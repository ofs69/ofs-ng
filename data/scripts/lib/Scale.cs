// Shipped library node. Copy this file into your own scripts folder to fork and edit it.
// !ofs:signal functional
// !ofs:input a
// !ofs:name Scale
// !ofs:description Multiplies the signal by a gain factor and adds an offset. Use after a generator to fit it into a position range.
// !ofs:param gain 1.0
// !ofs:param offset 0.0
return a * gain + offset;
