# wasthon shim for pygame.pkgdata: assets live in MEMFS at "/<identifier>"
# (written by the loader). font.c reads only `.name` (a path) + `.close()`;
# other callers may `.read()`. Replaces the upstream importlib.resources path.


def getResource(identifier, pkgname=__name__):
    return _Resource("/" + identifier)


class _Resource:
    def __init__(self, path):
        self.name = path
        self._f = None

    def read(self, *args):
        if self._f is None:
            self._f = open(self.name, "rb")
        return self._f.read(*args)

    def close(self):
        if self._f is not None:
            self._f.close()
            self._f = None
