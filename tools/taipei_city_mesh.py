"""Build the Xinyi city mesh in east/north/up metres around the Taipei 101 spire."""
import json
import math
import statistics
import zlib
from collections import defaultdict
from pathlib import Path

from mathutils.geometry import tessellate_polygon


DATA = Path(__file__).resolve().parent / 'data'
# OSM outline IDs identify the entire building, including its mapped building:part solids.
LANDMARKS = {
    'way/1159328965': 'taipei_101',
    'way/383731054': 'nanshan_plaza',
    'way/1339580862': 'the_sky_taipei',
    'way/378443967': 'cathay_landmark',
    'way/289834528': 'farglory_financial_center',
    'way/137077756': 'twtc_international_trade_building',
    'way/1010525370': 'fubon_life',
}


def signed_area(ring):
    return .5 * sum(a[0]*b[1] - b[0]*a[1] for a,b in zip(ring, ring[1:] + ring[:1]))


def city_mesh(mesh, relief_height):
    survey = json.loads((DATA/'taipei_xinyi_buildings.json').read_bytes())
    landmarks = {r['id']: r for r in json.loads((DATA/'taipei_xinyi_landmarks.json').read_bytes())['landmarks']}
    buildings = survey['buildings']
    groups = defaultdict(lambda: ([], []))
    children = defaultdict(list)
    local_floors = defaultdict(list)
    for b in buildings:
        if b.get('part') and b.get('parent_id'):
            children[b['parent_id']].append(b)
        elif b.get('levels') and 1 <= b['levels'] <= 15:
            x,y = b['outer'][0]
            local_floors[int(x//200),int(y//200)].append(b['levels'])
    medians = {k:statistics.median(v) for k,v in local_floors.items()}

    def volume(part, mat, rings, levels):
        """Ring stacks preserve concave parcel boundaries and any open courtyards."""
        verts, faces = groups[part,mat]
        base = len(verts)
        count = sum(map(len,rings))
        for pts in levels:
            verts.extend(pts)
        if len(rings)==1:
            caps = [tuple(range(count))]
        else:
            caps = tessellate_polygon(rings)
        for face in caps:
            faces.append(tuple(base+i for i in reversed(face)))
            faces.append(tuple(base+(len(levels)-1)*count+i for i in face))
        start = 0
        for ring in rings:
            for j in range(len(ring)):
                a,b = start+j, start+(j+1)%len(ring)
                for k in range(len(levels)-1):
                    idx = base + k*count
                    faces.append((idx+a,idx+b,idx+count+b,idx+count+a))
            start += len(ring)

    def solid(part, mat, rings, bottom, top):
        pts = [p for r in rings for p in r]
        volume(part,mat,rings,[[(x,y,z) for x,y in pts] for z in (bottom,top)])

    def fixture(part, mat, cx, cy, dx, dy, width, depth, z0, z1):
        ring = [(cx+dx*u-dy*v,cy+dy*u+dx*v) for u,v in
                ((-width/2,-depth/2),(width/2,-depth/2),(width/2,depth/2),(-width/2,depth/2))]
        solid(part,mat,[ring],z0,z1)

    for b in buildings:
        ident = b['id']
        parent = b.get('parent_id')
        landmark = LANDMARKS.get(parent or ident)
        outer = [tuple(p) for p in b['outer']]
        if signed_area(outer)<0:
            outer.reverse()
        holes = [[tuple(p) for p in r] for r in b.get('holes',[])]
        rings = [outer] + [list(reversed(r)) if signed_area(r)>0 else r for r in holes]
        pts = [p for r in rings for p in r]
        cx = sum(x for x,y in outer)/len(outer)
        cy = sum(y for x,y in outer)/len(outer)
        footprint = abs(signed_area(outer))
        # The existing hero is a metrically authored 101 tower with its observatory,
        # damper and spire. Keep the mapped low surrounding mall and entrance volumes.
        if landmark=='taipei_101' and (not b.get('part') or (math.hypot(cx,cy)<45 and
                ((b.get('height_m') or 0)>35 or (b.get('levels') or 0)>12))):
            continue
        if ident in children:
            # OSM building outlines envelope their parts. Drawing the parent too
            # would turn an entire mall/office parcel into a solid at the tower height.
            continue
        if not b.get('part') and footprint<9 and not b.get('height_m'):
            continue
        ground = max(0.,relief_height(cx,cy))
        bottom = ground + max(0.,b.get('min_height_m') or 3.1*(b.get('min_level') or 0))
        levels = b.get('levels')
        kind = b.get('kind','yes')
        height = b.get('height_m')
        if not height:
            floors = levels or (1 if kind in ('shed','garage','garages','roof','service') else
                                medians.get((int(cx//200),int(cy//200)),4))
            height = floors * (3.85 if kind in ('office','commercial','retail','hotel') else 3.1)
        if landmark and landmark!='taipei_101':
            ref = landmarks[landmark]
            target = ref.get('body_m',ref['height_m'])
            if parent and parent in children:
                # Parts with floor counts are vertically registered to their building's
                # independently published height. Explicit part heights stay in metres.
                if not b.get('height_m'):
                    tallest = max((p.get('levels') or 0 for p in children[parent]), default=0)
                    if levels and tallest:
                        height = levels * target / tallest
            else:
                height = target
        top = ground + height
        if top <= bottom + .2:
            continue
        part = 3 if cx<0 else 4
        seed = zlib.crc32(ident.encode())
        mat = 5 if height>80 or kind in ('office','commercial') else (13 if seed%3 else 14)
        if height<7 and kind not in ('retail','commercial'):
            mat = 2
        shape = b.get('roof_shape')
        if shape=='dome' and not holes:
            # Stadiums, church cupolas and the mall atrium share a tagged roof form.
            rise = b.get('roof_height_m') or min(.82*(top-bottom),.30*math.sqrt(footprint))
            eave = top-rise
            solid(part,mat,rings,bottom,eave)
            profile=[(eave+rise*math.sin(a),math.cos(a))
                     for a in (0,.26,.54,.80,1.06,1.31,1.55)]
            volume(part,11,rings,[[(cx+(x-cx)*s,cy+(y-cy)*s,z) for x,y in pts] for z,s in profile])
        elif landmark=='nanshan_plaza' and levels and levels>=47:
            # The architect's two folded outer blades rise toward their shared axis.
            inner_x = max(x for x,y in outer) if cx<232 else min(x for x,y in outer)
            width = max(x for x,y in outer)-min(x for x,y in outer)
            volume(part,14,rings,[[(x,y,bottom) for x,y in pts],[(x,y,top-28) for x,y in pts],
                                  [(x,y,top-22*abs(x-inner_x)/max(width,1)) for x,y in pts]])
        elif landmark=='the_sky_taipei' and height>200:
            # Slightly curved/pleated glass shaft on the mapped 47-storey tower plan.
            loft = [[(cx+(x-cx)*s,cy+(y-cy)*s,bottom+(top-bottom)*f) for x,y in pts]
                    for f,s in ((0,1),(.72,1),(.93,.99),(1,.91))]
            volume(part,14,rings,loft)
            for a,c in zip(outer,outer[1:]+outer[:1]):
                length = math.dist(a,c)
                if length<12: continue
                dx,dy=(c[0]-a[0])/length,(c[1]-a[1])/length
                bays=max(2,round(length/5))
                for k in range(bays):
                    t=(k+.5)/bays
                    x,y=a[0]*(1-t)+c[0]*t,a[1]*(1-t)+c[1]*t
                    ring=[(x+u*dx-v*dy,y+u*dy+v*dx) for u,v in ((-.27,-.55),(.27,-.55),(.27,.08),(-.27,.08))]
                    volume(part,11,[ring],[[(cx+(px-cx)*s,cy+(py-cy)*s,bottom+(top-bottom)*f) for px,py in ring]
                                            for f,s in ((.08,1),(.72,1),(.93,.99),(1,.91))])
        elif landmark=='fubon_life' and height>200:
            occupied = ground+landmarks[landmark]['occupied_m']
            volume(part,5,rings,[[(cx+(x-cx)*s,cy+(y-cy)*s,z) for x,y in pts]
                                 for z,s in ((bottom,1),(occupied,1),(occupied+2,.85),(top-2,.85))])
            solid(part,11,rings,top-2,top)
        else:
            solid(part,mat,rings,bottom,top)
        if landmark=='farglory_financial_center' and math.isclose(height,landmarks[landmark]['body_m']):
            # The lantern and mast reach 208.3 m above the 164.5 m main volume.
            h = ground + landmarks[landmark]['height_m']
            radius = min(9.,math.sqrt(footprint)*.35)
            rim = [(math.cos(i*math.tau/12),math.sin(i*math.tau/12)) for i in range(12)]
            geom = [[(cx+dx*r,cy+dy*r,z) for dx,dy in rim] for z,r in
                    ((top,radius),(top+7,radius*.88),(top+13,radius*.60),(top+17,radius*.25))]
            volume(part,15,[[q[:2] for q in geom[0]]],geom)
            fixture(part,11,cx,cy,1,0,1.15,1.15,top+17,h)

        # Visible local shops: retain mapped party walls and door elevations.
        # These small awnings and rooftop fixtures are artistic architectural details.
        if (b.get('part') or landmark or not 65<footprint<1550 or not 7<height<50 or
                abs(cx)>950 or not -850<cy<1300 or seed%8):
            continue
        edges = [(math.dist(a,c),a,c) for a,c in zip(outer,outer[1:]+outer[:1]) if c[0]>a[0]]
        if not edges: continue
        length,a,c = max(edges)
        if length < 5: continue
        dx,dy = (c[0]-a[0])/length,(c[1]-a[1])/length
        x,y=(a[0]+c[0])/2,(a[1]+c[1])/2
        fixture(part,11,x+.7*dy,y-.7*dx,dx,dy,min(12,length*.86),1.5,ground+3,ground+3.35)
        fixture(part,16 if seed%5==0 else 15,x+1.43*dy,y-1.43*dx,dx,dy,
                min(8,length*.57),.16,ground+2.96,ground+3.19)
        if length>12 and not holes:
            # An enclosure sits just inside the street edge, even on deep L-shaped lots.
            x,y=x-1.7*dy,y+1.7*dx
            fixture(part,2,x,y,dx,dy,3.8,2.5,top,top+1.2)
    for (part, mat),(verts,faces) in sorted(groups.items()):
        mesh('xinyi_surveyed_%d_%d'%(part,mat),verts,faces,mat,part,rough=.29 if mat in (5,13,14) else .42)
    mapped_roads(mesh,survey.get('roads',[]),relief_height)


def mapped_roads(mesh, roads, relief_height):
    """Joined road ribbons retain actual intersections, bearings and traffic UVs."""
    widths={'motorway':11.,'trunk':10.,'primary':9.,'secondary':8.,'tertiary':7.,
            'residential':5.,'unclassified':5.,'service':3.8}
    batches=defaultdict(lambda:([],[],[]))
    for road in roads:
        path=road['points']
        if road.get('tunnel') or len(path)<2: continue
        kind=road['kind'].removesuffix('_link')
        half=max(2.,min(14., (road.get('lanes') or 0)*1.65+.8)) if road.get('lanes') else widths.get(kind,5.)
        zip_edges=list(zip(path,path[1:]))
        lengths=[math.dist(a,b) for a,b in zip_edges]
        if not all(lengths): continue
        directions=[((b[0]-a[0])/l,(b[1]-a[1])/l) for (a,b),l in zip(zip_edges,lengths)]
        vx,vy=path[-1][0]-path[0][0],path[-1][1]-path[0][1]
        verts,faces,uvs=batches[7 if abs(vx)>abs(vy) else 8]
        first=len(verts)
        along=0.
        rank=list(widths).index(kind) if kind in widths else len(widths)
        deck=.25-.012*rank+(5.0*max(1,road.get('layer') or 0) if road.get('bridge') else 0.)
        for i,(x,y) in enumerate(path):
            before=directions[max(0,i-1)]; after=directions[min(i,len(directions)-1)]
            nx,ny=-(before[1]+after[1])*.5,(before[0]+after[0])*.5
            normal_size=math.hypot(nx,ny)
            if normal_size<.1: nx,ny=-after[1],after[0]
            else: nx,ny=nx/normal_size,ny/normal_size
            miter=half/max(.60,-nx*after[1]+ny*after[0])
            ground=max(0.,relief_height(x,y))+deck
            for h,side in ((ground-.10,-1),(ground-.10,1),(ground,-1),(ground,1)):
                verts.append((x+nx*miter*side,y+ny*miter*side,h))
                uvs.append((along*.08/2.8,side*half*.08/2.8))
            if i<len(lengths):
                a=first+4*i
                faces.extend([(a,a+1,a+5,a+4),(a+2,a+6,a+7,a+3),
                              (a,a+4,a+6,a+2),(a+1,a+3,a+7,a+5)])
                along+=lengths[i]
        end=first+4*(len(path)-1)
        faces.extend([(first,first+2,first+3,first+1),(end,end+1,end+3,end+2)])
    for part,(verts,faces,uvs) in sorted(batches.items()):
        obj=mesh('xinyi_mapped_avenues',verts,faces,1,part,rough=.20,preserve_uv=True)
        uv=obj.data.uv_layers.new(name='RoadUV')
        for loop in obj.data.loops:
            uv.data[loop.index].uv=uvs[loop.vertex_index]
