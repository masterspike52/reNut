# cmake -DIN=<file> -DOUT=<include> -P embed_string.cmake
#
# Writes the file as one C string literal of \x escapes, for
#   const char kData[] =
#   #include "<include>"
#       ;
# (its size is sizeof(kData) - 1). A string literal compiles far faster than a
# byte list for megabytes of data.

file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1" hex "${hex}")
file(WRITE "${OUT}" "\"${hex}\"\n")
