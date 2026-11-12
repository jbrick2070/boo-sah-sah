# Local Neural Models (`assets/models/`)

Milestone **M0** does not bundle or require any neural model binaries.

- **NPU / Madame N (Milestone M5)**: Will store the compact local mood-director model (16/32-feature audio+timeline input -> `Calm / Suspense / Bloom / Comic / Transition` + bounded mood scalars) alongside SHA-256 checksums and provenance metadata.
- **M0 Fallback**: Uses `boo::ScriptedDirector` (`src/inference/scripted_director.cpp`), an honest, deterministic non-neural stage director labeled `Substituted (Scripted_Deterministic_Director)`.
