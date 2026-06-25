// Shipped library node. Copy this file into your own scripts folder to fork and edit it.
// !ofs:signal functional
// !ofs:input  stroke
// !ofs:output main
// !ofs:output mirror
// !ofs:name Mirror
// !ofs:description Splits one motion into a primary output and its reflection about Center. Wire main and mirror to opposing axes (e.g. L0 and R0) to drive them from a single stroke.
// !ofs:param Center 50 0 100 int
// A multi-output node: the body assigns each named output instead of returning a value.
main = stroke;
mirror = Center * 2f - stroke; // reflected about Center → an opposing axis
