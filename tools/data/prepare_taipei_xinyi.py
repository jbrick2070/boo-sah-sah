"""Curate the three Overpass JSON responses described in TAIPEI-MAP-NOTICE.txt.

Usage: python prepare_taipei_xinyi.py INPUT_DIRECTORY OUTPUT.json
Build-time dependencies: pyproj 3.7 and Shapely 2.1. No network or game runtime dependencies.

To reproduce the inputs, run each Overpass QL query below on an instance with
historical data. Add [date:"<timestamp>"] to the first statement using the UTC
timestamp stored in the output's sources entry. Historical server responses can
have different JSON formatting or generators from the saved raw fingerprints.

buildings-overpass-api.de.json
  [out:json][timeout:120];(way["building"](25.015,121.544,25.053,121.586);
  relation["building"]["type"="multipolygon"](25.015,121.544,25.053,121.586););out geom;
parts-and-roads.json
  [out:json][timeout:120];(way["building:part"](25.015,121.544,25.053,121.586);
  relation["building:part"]["type"="multipolygon"](25.015,121.544,25.053,121.586);
  way["highway"~"^(motorway|trunk|primary|secondary|tertiary)(_link)?$"]
  (25.015,121.544,25.053,121.586););out geom;
northern-towers.json
  [out:json][timeout:100];(wr["building"]["height"](25.042,121.511,25.101,121.612);
  wr["building"]["building:levels"](25.042,121.511,25.101,121.612);
  wr["building:part"]["height"](25.042,121.511,25.101,121.612);)->.b;
  nwr.b(if:number(t["height"])>=35 || number(t["building:levels"])>=11);out geom;
"""

from collections import Counter
from functools import lru_cache
import hashlib
import json
import math
from pathlib import Path
import re
import sys

from pyproj import CRS, Transformer
from shapely import STRtree, make_valid
from shapely.geometry import LineString, MultiLineString, Polygon, box
from shapely.geometry.polygon import orient
from shapely.ops import linemerge


ORIGIN = [25.033668, 121.564810]
EXTENT = [-2000, -2000, 2000, 2000]
CONTEXT_EXTENT = [-5000, 1500, 4500, 7000]
CORE = box(*EXTENT)
CONTEXT = box(*CONTEXT_EXTENT)
LOCAL = CRS.from_proj4(
    f"+proj=aeqd +lat_0={ORIGIN[0]} +lon_0={ORIGIN[1]} +datum=WGS84 +units=m"
)
project = Transformer.from_crs("EPSG:4326", LOCAL, always_xy=True)


@lru_cache(maxsize=None)
def xy(lon, lat):
    return project.transform(lon, lat)


def coordinates(geometry):
    return [xy(p["lon"], p["lat"]) for p in geometry]


def components(geometry, geometry_type):
    if geometry.geom_type == geometry_type and not geometry.is_empty:
        yield geometry
    elif hasattr(geometry, "geoms"):
        for child in geometry.geoms:
            yield from components(child, geometry_type)


def rings(linework):
    if not linework:
        return []
    lines = MultiLineString(linework)
    merged = linemerge(lines)
    return [list(line.coords) for line in components(merged, "LineString") if line.is_closed]


def polygons(element):
    if element["type"] == "way":
        points = coordinates(element["geometry"])
        if len(points) >= 4 and points[0] == points[-1]:
            yield from components(make_valid(Polygon(points)), "Polygon")
        return
    members = element["members"]
    def boundaries(role):
        linework = [coordinates(m["geometry"]) for m in members
                    if m["type"] == "way" and m.get("role", "") in role and m.get("geometry")]
        return rings(linework)
    shells = [Polygon(ring) for ring in boundaries({"outer", ""})]
    holes = [[] for _ in shells]
    for hole in boundaries({"inner"}):
        point = Polygon(hole).representative_point()
        matches = [i for i, shell in enumerate(shells) if shell.covers(point)]
        if matches:
            index = min(matches, key=lambda i: shells[i].area)
            holes[index].append(hole)
    for shell, voids in zip(shells, holes):
        yield from components(make_valid(Polygon(shell.exterior, voids)), "Polygon")


def numeric(value, units=False):
    if not value:
        return None
    match = re.fullmatch(r"\s*(-?\d+(?:\.\d+)?)\s*(m|ft)?\s*", str(value))
    if not match or (match[2] and not units):
        return None
    number = float(match[1]) * (0.3048 if match[2] == "ft" else 1)
    return int(number) if number.is_integer() else round(number, 3)


def source_id(element):
    return f'{element["type"]}/{element["id"]}'


def packed(points, close=False):
    result = []
    for x, y in points:
        pair = [round(x, 1), round(y, 1)]
        if not result or result[-1] != pair:
            result.append(pair)
    if close and result[0] == result[-1]:
        result.pop()
    return result


def extract_buildings(elements):
    rows = []
    geometries = []
    consumed = set()
    for element in elements.values():
        if element["type"] == "relation" and element["tags"].get("type") == "multipolygon":
            consumed.update(f'way/{m["ref"]}' for m in element["members"]
                            if m["type"] == "way" and m.get("role") == "outer")
    for eid, element in sorted(elements.items()):
        tags = element["tags"]
        if not ("building" in tags or "building:part" in tags) or eid in consumed:
            continue
        part = tags.get("building:part", "no") != "no"
        kind = tags.get("building:part" if part else "building")
        if kind in (None, "no") or tags.get("location") == "underground" or tags.get("level", "") == "-1":
            continue
        height = numeric(tags.get("height"), units=True)
        levels = numeric(tags.get("building:levels"))
        if height is not None and height <= 0:
            continue
        parts = sorted(polygons(element), key=lambda p: (-p.area, p.bounds))
        for index, poly in enumerate(parts):
            center = poly.representative_point()
            context = not CORE.covers(center)
            if context and (not CONTEXT.covers(center) or ((height or 0) < 35 and (levels or 0) < 11)):
                continue
            if poly.area < 1:
                continue
            tolerance = 0.15 if poly.area < 30 else (0.35 if math.hypot(center.x, center.y) < 700 else 0.75)
            simple = orient(poly.simplify(tolerance, preserve_topology=True), sign=1)
            outer = packed(simple.exterior.coords, close=True)
            voids = [packed(hole.coords, close=True) for hole in simple.interiors]
            if len(outer) < 3 or any(len(hole) < 3 for hole in voids):
                continue
            check = Polygon(outer, voids)
            if not check.is_valid or check.area < 1:
                continue
            row = {"id": eid if index == 0 else f"{eid}:{index+1}",
                   "kind": kind, "part": part, "outer": outer, "holes": voids,
                   "height_m": height, "levels": levels,
                   "min_height_m": numeric(tags.get("min_height"), units=True),
                   "min_level": numeric(tags.get("building:min_level"))}
            if context:
                row["context"] = True
            optional = {
                "name": tags.get("name:en") or tags.get("name"), "wikidata": tags.get("wikidata"),
                "roof_shape": tags.get("roof:shape"), "roof_height_m": numeric(tags.get("roof:height"), True),
                "roof_levels": numeric(tags.get("roof:levels")), "roof_direction": tags.get("roof:direction")
            }
            row.update({key: value for key, value in optional.items() if value is not None})
            rows.append(row)
            geometries.append(check)
    indices = [i for i, row in enumerate(rows) if not row["part"]]
    shapes = [geometries[i] for i in indices]
    tree = STRtree(shapes)
    for row, geometry in zip(rows, geometries):
        if not row["part"]:
            continue
        matches = []
        for j in tree.query(geometry):
            parent = shapes[j]
            if parent.buffer(0.75).covers(geometry):
                matches.append((parent.area, rows[indices[j]]["id"]))
        if matches:
            row["parent_id"] = min(matches)[1]
    return rows


def extract_roads(elements):
    rows = []
    for eid, item in sorted(elements.items()):
        tags = item["tags"]
        if item["type"] != "way" or "highway" not in tags or tags.get("area") == "yes":
            continue
        line = LineString(coordinates(item["geometry"]))
        for i, piece in enumerate(components(line.intersection(CORE), "LineString")):
            points = packed(piece.simplify(0.35).coords)
            if len(points) < 2:
                continue
            rows.append({"id": eid if i == 0 else f"{eid}:{i+1}", "kind": tags["highway"],
                         "points": points, "name": tags.get("name:en") or tags.get("name"),
                         "bridge": tags.get("bridge", "no") != "no", "tunnel": tags.get("tunnel", "no") != "no",
                         "layer": numeric(tags.get("layer")) or 0, "lanes": numeric(tags.get("lanes"))})
    return rows


def main(source_dir, output_file):
    elements = {}
    sources = []
    for name in ("buildings-overpass-api.de.json", "parts-and-roads.json", "northern-towers.json"):
        raw = (source_dir / name).read_bytes()
        data = json.loads(raw)
        if data.get("remark") or not data.get("elements"):
            raise ValueError(f"Incomplete Overpass response: {name}")
        sources.append({"file": name, "timestamp": data["osm3s"]["timestamp_osm_base"],
                        "sha256": hashlib.sha256(raw).hexdigest()})
        for item in data["elements"]:
            elements.setdefault(source_id(item), item)
    rows = extract_buildings(elements)
    roads = extract_roads(elements)
    result = {"format": 1, "origin_latlon": ORIGIN, "crs": "WGS84 local azimuthal equidistant, metres east/north",
              "extent_m": EXTENT, "context_extent_m": CONTEXT_EXTENT,
              "attribution": "© OpenStreetMap contributors", "license": "ODbL-1.0",
              "sources": sources, "buildings": rows, "roads": roads}
    contents = json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\r\n"
    output_file.write_bytes(contents.encode("utf-8"))
    counts = Counter()
    for row in rows:
        category = ("context" if row.get("context") else "core") + (" parts" if row["part"] else " buildings")
        counts[category] += 1
        field = "height" if row["height_m"] is not None else "floors only" if row["levels"] else "unknown"
        counts[f"{category}: {field}"] += 1
    print(json.dumps(dict(sorted(counts.items())), indent=2), f"{len(roads)} road lines")


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]))
