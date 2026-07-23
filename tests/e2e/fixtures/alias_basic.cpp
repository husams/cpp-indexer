// Type aliases over a plain struct: a C-style typedef, a C++ using-alias,
// and an alias whose target is itself an alias. No templates. Every alias
// must be a symbol of its own, keep its written underlying type, and hold a
// `uses` edge to the declaration it names -- one alias-declaration step at a
// time, so the chain Pigment -> Paint -> Color stays visible.

struct Color {
  int red;
  int green;
  int blue;
};

typedef Color Paint;

using Rgb = Color;

using Pigment = Paint;

Rgb blend(Paint base, Pigment tint);
