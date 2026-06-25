// Shipped library node. Copy this file into your own scripts folder to fork and edit it.
// !ofs:signal functional
// !ofs:input a
// !ofs:name Mix
// !ofs:description Blends the signal toward a constant target position. Amount 0 = unchanged, 1 = fully replaced by target.
// !ofs:param amount 0.5 0 1
// !ofs:param target 50 0 100 int
return a + amount * (target - a);
