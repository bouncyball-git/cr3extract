Utility for extracting embedded JPEGs from Canon CR3 raw file and copy exif from the original.
```
Usage: ./cr3extract <source.CR3> [-] [-v] [-m] [-j all|1|2|3] [-o FILENAME] [-h]
Options:
  (no -j) : Extract largest JPEG preview unaltered (no EXIF changes) to file or stdout
  -       : Output to stdout (allowed in default mode and -j 1|2|3)
  -v      : Verbose output
  -m      : Minimize EXIF data (applies only with -j options)
  -j all  : Extract first 3 JPEG segments with full/minimized EXIF (stdout not allowed)
  -j 1    : Extract 1st JPEG segment with full/minimized EXIF (stdout allowed)
  -j 2    : Extract 2nd JPEG segment with full/minimized EXIF (stdout allowed)
  -j 3    : Extract 3rd JPEG segment with full/minimized EXIF (stdout allowed)
  -o FILENAME : Specify output file name. In default mode or -j 1|2|3, FILENAME is used exactly.
                In -j all mode, FILENAME is used as a base name with an index appended.
  -h      : Print this help message and exit
```
