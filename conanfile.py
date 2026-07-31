from conan import ConanFile
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout


class ClogxConan(ConanFile):
    name = "clogx"
    version = "0.2.0"
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
