</$objtype/mkfile

DIRS=libutil libutf

all:V:
	for (i in $DIRS)
		@{ cd $i; mk }
		mk -f mkfile_bin

install:V:
	for (i in $DIRS)
		@{ cd $i; mk $target }
		mk -f mkfile_bin $target

clean:V:
	for (i in $DIRS)
		@{ cd $i; mk $target }
		mk -f mkfile_bin $target

nuke:V:
	for (i in $DIRS)
		@{ cd $i; mk $target }
		mk -f mkfile_bin $target


