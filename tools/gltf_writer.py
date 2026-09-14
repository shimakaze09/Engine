# Shared glTF 2.0 buffer assembly for the asset generators (gen_character,
# gen_props): one binary blob per document, buffer views appended at 4-byte
# alignment, and accessors indexed over them. Owning one copy here keeps
# the generators' output byte-identical to each other's layout rules, so a
# change to view alignment or accessor shape reaches every bundled asset.


class GltfBufferBuilder:
    """Accumulates one glTF buffer: its bytes, the views that partition it,
    and the accessors that type those views. Views start on a 4-byte
    boundary (glTF requires it for every component type the generators
    emit); accessors carry min/max only when the caller supplies them,
    which glTF mandates for POSITION alone."""

    def __init__(self):
        self.blob = bytearray()
        self.buffer_views = []
        self.accessors = []

    def align(self, n):
        while len(self.blob) % n:
            self.blob.append(0)

    def add_view(self, data, target=None):
        """Appends data as a new buffer view and returns its index."""
        self.align(4)
        offset = len(self.blob)
        self.blob.extend(data)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
        if target:
            view["target"] = target
        self.buffer_views.append(view)
        return len(self.buffer_views) - 1

    def add_accessor(self, view, ctype, count, atype, vmin=None, vmax=None):
        """Declares an accessor over a view and returns its index."""
        acc = {"bufferView": view, "componentType": ctype, "count": count,
               "type": atype}
        if vmin is not None:
            acc["min"] = vmin
            acc["max"] = vmax
        self.accessors.append(acc)
        return len(self.accessors) - 1
