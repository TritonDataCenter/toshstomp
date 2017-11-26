
all:	toshstomp toshreplay

toshstomp: toshstomp.c
	gcc -m64 -Wall -Werror -Wextra -o toshstomp toshstomp.c

toshreplay: toshreplay.c
	gcc -m64 -Wall -Werror -Wextra -o toshreplay toshreplay.c


.PHONY: clean
clean:
	rm -f toshstomp toshreplay
