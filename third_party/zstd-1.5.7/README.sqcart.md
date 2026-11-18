# zstd 1.5.7, the decoder

`zstddeclib.c` is zstd's single-file decompressor (built with
`build/single_file_libs/combine.sh`), the same copy sqterm carries
(`sq_app/third_party/zstd`). sqcart's 3.0 reader decodes codec 1 and 2
entries and chunks with it (format 3.0 §4.3). BSD licence (`LICENSE`).
There is no compressor here: the 3.0 writer, when it comes, adds one.
