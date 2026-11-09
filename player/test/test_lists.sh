# library.c and spotmatch.c built on their own, spotify.c inside test_lists.c; run in a scratch sdmc:/ tree
set -e
S=$(pwd)
rm -rf /tmp/tl && mkdir -p /tmp/tl && cd /tmp/tl
F="-O1 -g -Wall -Wno-format -Wno-unused-result -fsanitize=address,undefined -I$S/player/test/fake_meta -I$S/player/source"
gcc $F -c $S/player/source/library.c -o library.o
gcc $F -c $S/player/source/spotmatch.c -o spotmatch.o
gcc $F $S/player/test/test_lists.c library.o spotmatch.o -ljansson -lm -o t
./t
