# builds the old reader (renamed) and the new one into test_tags.c, and runs it over the folders given
set -e
mkdir -p /tmp/tt
sed 's/\btags_read\b/tags_read_old/; s/\bgenre_name\b/genre_name_old/g' player/source/tags.c.pre28 > /tmp/tt/old.c
gcc -O1 -w -Iplayer/test/fake -Iplayer/source -c /tmp/tt/old.c -o /tmp/tt/old.o
gcc -O1 -Wall -fsanitize=address,undefined -Iplayer/test/fake -Iplayer/source -c player/source/tags.c -o /tmp/tt/new.o
gcc -O1 -Wall -Iplayer/test/fake -Iplayer/source -fsanitize=address,undefined player/test/test_tags.c /tmp/tt/old.o /tmp/tt/new.o -lm -o /tmp/tt/t
/tmp/tt/t "$@"
