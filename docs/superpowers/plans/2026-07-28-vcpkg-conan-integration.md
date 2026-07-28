# vcpkg and Conan Package Manager Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provide official `vcpkg.json` manifest and Conan 2.0 `conanfile.py` recipe for `clogx`.

**Architecture:** Add `vcpkg.json` with features `tls` and `system-yaml`. Add `conanfile.py` recipe supporting options (`shared`, `with_tls`, `with_yaml`). Update `README.md` and `CMakeLists.txt` for seamless dependency resolution.

**Tech Stack:** vcpkg, Conan 2.0+, CMake, Python 3, JSON.

---

### Task 1: Add `vcpkg.json` Manifest and `conanfile.py` Recipe

**Files:**
- Create: `vcpkg.json`
- Create: `conanfile.py`

- [ ] **Step 1: Create `vcpkg.json`**

```json
{
  "name": "clogx",
  "version": "0.1.0",
  "description": "Lightweight C99 logging library with multi-sink, async queue, log rotation, JSON output, and TLS support",
  "homepage": "https://github.com/quintin-lee/clogx",
  "license": "MIT",
  "dependencies": [],
  "features": {
    "tls": {
      "description": "Enable OpenSSL TLS support for socket sink",
      "dependencies": [
        "openssl"
      ]
    },
    "system-yaml": {
      "description": "Use system libyaml package instead of bundled source",
      "dependencies": [
        "libyaml"
      ]
    }
  }
}
```

- [ ] **Step 2: Create `conanfile.py`**

```python
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class ClogxConan(ConanFile):
    name = "clogx"
    version = "0.1.0"
    license = "MIT"
    author = "quintin"
    url = "https://github.com/quintin-lee/clogx"
    description = "Lightweight C99 logging library with multi-sink, async queue, log rotation, JSON output, and TLS support"
    topics = ("logging", "c99", "json", "async", "tls")
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_tls": [True, False],
        "with_yaml": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_tls": False,
        "with_yaml": False,
    }

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")
        self.settings.rm_safe("compiler.libcxx")
        self.settings.rm_safe("compiler.cppstd")

    def requirements(self):
        if self.options.with_yaml:
            self.requires("libyaml/0.2.5")
        if self.options.with_tls:
            self.requires("openssl/3.1.2")

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["CLOG_BUILD_SHARED"] = self.options.shared
        tc.variables["CLOG_ENABLE_TLS"] = self.options.with_tls
        tc.variables["CLOG_USE_SYSTEM_YAML"] = self.options.with_yaml
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["clogx"]
```

- [ ] **Step 3: Validate syntax**

Run: `python3 -m json.tool vcpkg.json > /dev/null && python3 -m py_compile conanfile.py`
Expected: PASS with 0 syntax errors.

- [ ] **Step 4: Commit**

```bash
git add vcpkg.json conanfile.py
git commit -m "feat: add vcpkg.json manifest and conanfile.py recipe"
```

---

### Task 2: Update `CMakeLists.txt` & Documentation

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `README.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Update `CMakeLists.txt` to support package manager dependency resolution**

Ensure `find_package(yaml-0.1)` fallback to `find_package(yaml)` / `find_package(YAML)` works when `CLOG_USE_SYSTEM_YAML=ON`.

- [ ] **Step 2: Update `README.md` & `CHANGELOG.md`**

Document `vcpkg` and `Conan 2.0` installation and usage instructions in `README.md` and `CHANGELOG.md`.

- [ ] **Step 3: Verify build**

Run: `make check`
Expected: ALL PASS

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt README.md CHANGELOG.md
git commit -m "docs: add vcpkg and Conan usage documentation"
```
