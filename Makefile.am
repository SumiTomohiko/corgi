
SUBDIRS = src
ACLOCAL_AMFLAGS = -I m4

doc:
	if [ ! -h doc/index.rst ]; then \
		ln -s `pwd`/README `pwd`/doc/index.rst; \
	fi
	cd doc && make html

.PHONY: doc

# vim: tabstop=8 shiftwidth=8 noexpandtab
