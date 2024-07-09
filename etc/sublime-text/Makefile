SRCS := \
	Compis.sublime-syntax \
	Build.sublime-build \
	$(wildcard *.tmPreferences) \
	$(wildcard Snippets/*.sublime-snippet) \

all: Compis.sublime-package

Compis.sublime-package: $(SRCS)
	zip -q -X -r "$@" $^
