# Regenerate all wiki diagrams.
#   make           -> build everything (dot + sequences)
#   make dot       -> Graphviz .dot -> .png   (native, works on aarch64)
#   make seq-image -> build the mermaid render container (once)
#   make seq       -> Mermaid  .mmd -> .png   (via container; see note)
#   make clean
#
# Mermaid sequence PNGs render through a container (render.Dockerfile). This host
# is aarch64 + glibc 2.26 (Amazon Linux 2): Chrome-for-Testing ships no linux-arm64
# build and modern chromium needs glibc >= 2.28, so a native mmdc cannot launch a
# browser here. The image carries Debian's arm64 chromium + its own libc.
# Build it once with `make seq-image`, then `make seq`.
#
# To render natively instead (on a host that has a working Chrome), override:
#   MERMAID="mmdc -p puppeteer-config.json" make seq
DOT    ?= dot
DOCKER ?= docker
IMAGE  ?= mermaid-render:local
# Graphviz resolves the label font (Helvetica) through fontconfig. When
# FONTCONFIG_PATH is unset, some environments log "Fontconfig error: Cannot load
# default config file" and silently fall back to a symbol font -- the diagram
# renders with correct boxes and edges but every label comes out as dingbats.
# Pin it so a bad render cannot be mistaken for a good one. Override if your
# fontconfig lives elsewhere.
FONTCONFIG_PATH ?= /etc/fonts
export FONTCONFIG_PATH
# SCALE = device-pixel multiplier for crisp (hi-DPI) PNGs; BG = page background.
SCALE  ?= 3
BG     ?= white
# Default mermaid renderer = run mmdc inside the container, cwd-mounted at /data.
MERMAID ?= $(DOCKER) run --rm -u $$(id -u):$$(id -g) -e HOME=/tmp -v "$$PWD":/data $(IMAGE)

DOT_PNG := $(patsubst %.dot,%.png,$(wildcard *.dot))
MMD_PNG := $(patsubst %.mmd,%.png,$(wildcard *.mmd))

all: dot seq
dot: $(DOT_PNG)
seq: $(MMD_PNG)

seq-image:
	$(DOCKER) build -f render.Dockerfile -t $(IMAGE) .

%.png: %.dot
	$(DOT) -Tpng $< -o $@

%.png: %.mmd puppeteer-config.json
	$(MERMAID) -i $< -o $@ -p puppeteer-config.json -s $(SCALE) -b $(BG)

clean:
	rm -f $(DOT_PNG) $(MMD_PNG)

.PHONY: all dot seq seq-image clean
