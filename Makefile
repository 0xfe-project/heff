# heff's development commands, each a name for what does the work: a
# step of build.zig (the building, by zig alone), or a script of test/,
# bench/, editors/.  `make` lists them.
#
# GNU make (3.81, macOS's, or newer).  Each script decides for itself
# how much to run at once, so the targets run one after another, even
# under `make -j`.

ZIG ?= zig
NODE ?= node
# (the heff fmt runs, and the tests: HEFF, given
# on the command line or in the environment, else the one built)
HEFF ?= .heff/out/heff
# (the .hf files laid out by `heff fmt`: the tests' are left as they are,
# their offsets and broken ones the tests', as .pre-commit-config.yaml)
HF_FILES = $(shell git ls-files '*.hf' ':!test/')
VSCODE = editors/vscode
VSIX = $(VSCODE)/heff-$(shell $(NODE) -p "require('./$(VSCODE)/package.json').version").vsix

GROUPS ?=##      test: the groups of test/run.mjs (all when empty)
JOBS ?=##        test-asan: how many at once (HEFF_ASAN_JOBS)
TIMEOUT ?=##     test-asan: seconds each check is given (HEFF_ASAN_TIMEOUT)
GEN ?= 60 1##    sanitize, tsan: HEFF_GEN of test/backends.hf, N SEED: programs generated and their random seed
RUN ?=##         golden: the goldens updated, heff test's --run (all when empty)
TARGETS ?=##     native: zig targets, e.g. "x86_64-linux-musl aarch64-macos"
BENCH ?=##       bench: bench/run.mjs's arguments, e.g. "--runs 5 pure"

.NOTPARALLEL:
.PHONY: help build build-seed boot native release test test-compiler test-lsp \
  test-backends test-fixpoint test-asan check golden sanitize tsan fmt \
  fmt-check aliases aliases-check bat bat-check vsix install install-all bench clean

help: ## this list
	@echo "make TARGET [VARIABLE=VALUE...]"
	@echo "(zig and make build heff; Node.js runs the tests, the benchmarks and the editors' targets)"
	@awk 'BEGIN { FS = ":.*## " } \
	  /^##@ / { printf "\n%s\n", substr($$0, 5) } \
	  /^[a-z][a-z-]*:.*## / { printf "  %-15s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo
	@echo "Variables"
	@awk 'BEGIN { FS = " *\\?= *|## *" } \
	  /^[A-Z]+ \?=.*##/ { printf "  %-15s %s%s\n", $$1, $$3, ($$2 == "" ? "" : " [" $$2 "]") }' $(MAKEFILE_LIST)

##@ Building (by zig alone)

build: ## .heff/out/heff, built by the heff built before (from the seed, the first time)
	$(ZIG) build

build-seed: ## .heff/out/heff, built from the seed (boot/heff.tar.xz), always
	$(ZIG) build seed

boot: ## renew the seed, boot/heff.tar.xz, once heff compiles itself to itself (and xz)
	$(ZIG) build boot

native: ## heff for other systems, .heff/out/native/TARGET/ (TARGETS=...)
	$(ZIG) build native -Dtargets="$(TARGETS)"

release: ## an archive of heff for each platform, and SHA256SUMS, .heff/out/release/
	$(ZIG) build release

##@ Testing (on the heff built: make build first; Node.js but test-fixpoint)

test: ## the test groups, all at once (GROUPS=..., all and the fixpoint by default)
	$(NODE) test/run.mjs $(GROUPS)
	$(if $(GROUPS),,$(ZIG) build fixpoint)

test-compiler: ## elaborating, formatting, the cache, the command line, the goldens
	$(NODE) test/run.mjs compiler

test-lsp: ## the language server
	$(NODE) test/run.mjs lsp

test-backends: ## each backend held to the interpreter, the example apps
	$(NODE) test/run.mjs backends

test-fixpoint: ## heff compiles itself to the same C (by zig, as make boot checks)
	$(ZIG) build fixpoint

test-asan: ## heff under AddressSanitizer, by clang (JOBS=..., TIMEOUT=...)
	$(if $(JOBS),HEFF_ASAN_JOBS=$(JOBS) )$(if $(TIMEOUT),HEFF_ASAN_TIMEOUT=$(TIMEOUT) )$(NODE) test/native-asan.mjs

check: build-seed test fmt-check ## all a change is held to: from the seed, every test, the layout

golden: ## write the golden texts anew, after an intended change (RUN=...)
	$(HEFF) test --update $(if $(RUN),--run '$(RUN)' )test/golden.hf

sanitize: ## the backends' C programs under AddressSanitizer and UBSan (GEN=...)
	HEFF_SANITIZE=1 HEFF_GEN="$(GEN)" $(HEFF) test test/backends.hf

tsan: ## the backends, the threads test under ThreadSanitizer too (GEN=...)
	HEFF_TSAN=1 HEFF_GEN="$(GEN)" $(HEFF) test test/backends.hf

bench: ## the benchmarks, timed (BENCH=...)
	$(NODE) bench/run.mjs $(BENCH)

##@ Formatting

fmt: ## lay out the .hf files in place, as heff fmt does
	@echo "$(HEFF) fmt \$$(git ls-files '*.hf' ':!test/')"
	@$(HEFF) fmt $(HF_FILES)

fmt-check: ## name the .hf files not laid out so
	@echo "$(HEFF) fmt --check \$$(git ls-files '*.hf' ':!test/')"
	@$(HEFF) fmt --check $(HF_FILES)

aliases: ## each import's alias the word scripts/aliases.hf's table says (#60)
	HEFF=$(HEFF) $(HEFF) run scripts/aliases.hf

aliases-check: ## which aliases are not yet the table's words, no file written
	HEFF=$(HEFF) $(HEFF) run scripts/aliases.hf --check

##@ Editors

bat: ## editors/bat/heff.sublime-syntax, made from the VS Code grammar
	$(NODE) editors/bat/build.mjs

bat-check: ## is bat's syntax made from the grammar as it is?
	$(NODE) editors/bat/build.mjs --check

# (npm ci where the packages are not installed, or the lock changed:
# npm keeps its own lock, of what it installed, in node_modules/)
$(VSCODE)/node_modules/.package-lock.json: $(VSCODE)/package-lock.json
	cd $(VSCODE) && npm ci

vsix: $(VSCODE)/node_modules/.package-lock.json ## the VS Code extension, editors/vscode/heff-X.Y.Z.vsix
	cd $(VSCODE) && npm run package

##@ Installing

install: build ## heff into ~/.heff (bin/heff, its library), whatever HEFF_HOME says
	env -u HEFF_HOME .heff/out/heff install

install-all: install vsix ## install, and the VS Code extension, and bat's cache again
	@if command -v code >/dev/null; then code --install-extension $(VSIX); \
	  else echo "make: no code on the PATH; the extension is $(VSIX)"; fi
	@if command -v bat >/dev/null; then bat cache --build; \
	  else echo "make: no bat on the PATH"; fi

##@ Cleaning

# (the parts zig built, .heff/out/heff.o/ and .heff/out/native.o/, and
# those built under ASan, .heff/ref/native-asan.o/, each kept by what it
# is made of, are caches, as .heff/vN/ and .heff/bN/ are, and are kept)
clean: ## what the builds and tests made (not the caches, nor the seed)
	rm -rf .heff/bin .heff/build .heff/run .heff/examples $(VSCODE)/heff-*.vsix
	if [ -d .heff/out ]; then find .heff/out -mindepth 1 -maxdepth 1 ! -name heff.o ! -name native.o -exec rm -rf {} +; fi
	if [ -d .heff/ref ]; then find .heff/ref -mindepth 1 -maxdepth 1 ! -name native-asan.o -exec rm -rf {} +; fi
