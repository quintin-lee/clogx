# Design Spec: vcpkg and Conan Package Manager Support

**Date**: 2026-07-28  
**Topic**: Integration of vcpkg Manifest (`vcpkg.json`) and Conan 2.0 Recipe (`conanfile.py`)  
**Status**: Approved  

---

## 1. Overview

This design adds official package manager manifests for `vcpkg` and `Conan 2.0+` to `clogx`. It enables C/C++ developers to consume `clogx` as a dependency in modern C/C++ package management ecosystems.

---

## 2. Component Design

### 2.1 vcpkg Manifest (`vcpkg.json`)

Created at project root defining package metadata and optional feature dependencies:

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

### 2.2 Conan 2.0 Recipe (`conanfile.py`)

Created at project root for Conan 2.0+ package creation and consumption:

```python
from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout

class ClogxConan(ConanFile):
    name = "clogx"
    version = "0.1.0"
    license = "MIT"
    author = "quintin"
    url = "https://github.com/quintin-lee/clogx"
    description = "Lightweight C99 logging library"
    topics = ("logging", "c99", "json", "async", "tls")
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_tls": [True, False],
        "with_yaml": [True, False]
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_tls": False,
        "with_yaml": False
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

### 2.3 `CMakeLists.txt` Compatibility

Enhance `CMakeLists.txt` package discovery so `find_package(yaml-0.1)` / `find_package(yaml)` works seamlessly with vcpkg and Conan generated toolchains.

---

## 3. Verification Plan

1. **JSON Validation**: Verify `vcpkg.json` syntax with `python3 -m json.tool vcpkg.json`.
2. **Python Syntax Validation**: Verify `conanfile.py` compilation with `python3 -m py_compile conanfile.py`.
3. **CMake Integration**: Test building with `cmake -B build_cmake` ensuring zero CMake regressions.
