# Adds the sanitizer runtime to the native test link (PlatformIO passes build_flags to
# the compiler only).
Import("env")  # noqa: F821
env.Append(LINKFLAGS=["-fsanitize=address,undefined"])  # noqa: F821
