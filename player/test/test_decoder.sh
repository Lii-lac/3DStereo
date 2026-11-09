# the old decoder (renamed) and the new one, with the libraries, into test_decoder.c; run on the files given
set -e
mkdir -p /tmp/td
sed 's/\bdec_open\b/old_dec_open/g; s/\bdec_close\b/old_dec_close/g; s/\bdec_read\b/old_dec_read/g; s/\bdec_seek\b/old_dec_seek/g; s/\bfmt_of\b/old_fmt_of/g; s/\bfmt_name\b/old_fmt_name/g; s/\bfmt_lossless\b/old_fmt_lossless/g' player/source/decoder.c.pre28 > /tmp/td/old.c
F="-O1 -w -fsanitize=address,undefined -Iplayer/test/fake -Iplayer/source -I/usr/include/opus"
gcc $F -c /tmp/td/old.c -o /tmp/td/old.o
gcc $F -Wall -c player/source/decoder.c -o /tmp/td/new.o
gcc $F -c player/source/flac.c -o /tmp/td/flac.o
gcc $F -c player/source/vorbis.c -o /tmp/td/vorbis.o
gcc $F player/test/test_decoder.c /tmp/td/old.o /tmp/td/new.o /tmp/td/flac.o /tmp/td/vorbis.o -lopusfile -lopus -logg -lm -o /tmp/td/t
/tmp/td/t "$@"
