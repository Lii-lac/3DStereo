# a scratch sdmc:/ with glaive's cached page and pictures off the card, then test_pagefast
set -e
S=$(pwd)
rm -rf /tmp/pf && mkdir -p "/tmp/pf/sdmc:/3ds/player/artists/img" && cd /tmp/pf
cp $S/player/test/data/card/*.json "sdmc:/3ds/player/artists/"
cp $S/player/test/data/card/img/* "sdmc:/3ds/player/artists/img/"
gcc -O1 -Wall -Wno-format -Wno-unused-result -fsanitize=address,undefined -I$S/player/test/fake_meta -I$S/player/source $S/player/test/test_pagefast.c $S/player/source/spotmatch.c -ljansson -lm -o t
ASAN_OPTIONS=detect_leaks=0 ./t
