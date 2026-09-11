# Parser fuzzing

`make parser-check` runs deterministic truncation and byte mutations against
reference, Basic/Bearer, descriptor, index, manifest and operation-document
parsers. Successful operation documents must round-trip canonically.

`make fuzz` uses the same entry point with Clang libFuzzer and ASan/UBSan,
seeding from `corpus/` and writing coverage inputs to `BUILD/fuzz-corpus`.
The default campaign lasts 30 seconds. It makes no network requests and never
writes an OCI store. Linux Clang's sanitizer runtime is available in
`tests/Dockerfile`; Apple Clang installations without `libclang_rt.fuzzer_osx.a`
can run the mutation gate and sanitizers locally and this campaign in Docker.
The fixture strings are generated test data under this repository's license.
