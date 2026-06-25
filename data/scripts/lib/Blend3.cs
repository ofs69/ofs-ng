// Shipped library node. Copy this file into your own scripts folder to fork and edit it.
// !ofs:signal functional
// !ofs:input in0
// !ofs:input in1
// !ofs:input in2
// !ofs:name Blend 3
// !ofs:description Weighted average of three input signals (in0, in1, in2). Each weight scales one input; equal weights give a plain average. Leave an input unwired to feed it the neutral 50.
// !ofs:param weightA 1.0 0 4
// !ofs:param weightB 1.0 0 4
// !ofs:param weightC 1.0 0 4
float wsum = weightA + weightB + weightC;
return wsum == 0f ? 50f : (in0 * weightA + in1 * weightB + in2 * weightC) / wsum;
