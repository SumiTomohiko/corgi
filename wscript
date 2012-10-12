
from re import search

def read_version():
    with open("README.rst") as fp:
        for line in fp:
            m = search(r"\d+\.\d+\.\d+$", line)
            if m is None:
                continue
            return m.group()

APPNAME = "corgi"
VERSION = read_version()

def options(ctx):
    ctx.load("compiler_c")

def add_config_prefix(name):
    return "CORGI_" + name

def check_header(ctx, name):
    define_name = add_config_prefix(name.upper().replace(".", "_"))
    ctx.check(header_name=name, define_name=define_name, mandatory=False)

def configure(ctx):
    options(ctx)
    check_header(ctx, "alloc.h")
    ctx.define(add_config_prefix("PACKAGE_VERSION"), VERSION)
    ctx.write_config_header("include/corgi/config.h")

def build(ctx):
    ctx.recurse("src")

def set_algo(ctx):
    ctx.algo = "tar.xz"

def dist(ctx):
    set_algo(ctx)
    ctx.excl = [".*", "build"]

def distcheck(ctx):
    set_algo(ctx)

# vim: tabstop=4 shiftwidth=4 expandtab softtabstop=4 filetype=python
