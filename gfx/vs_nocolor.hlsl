float4 k : register(c0); /* x = 2/layoutW, y = 2/layoutH */
float4 a : register(c1); /* per-draw sprite zoom: x,y = anchor (sprite centre, layout px), z,w = x/y zoom */
struct VI { float4 p : POSITION; float4 c : COLOR0; float2 t0 : TEXCOORD0; float2 t1 : TEXCOORD1; };
struct VO { float4 p : POSITION; float4 c : COLOR0; float2 t0 : TEXCOORD0; float2 t1 : TEXCOORD1; };
VO main(VI i) {
  VO o;
  float px = a.x + (i.p.x - a.x) * a.z;
  float py = a.y + (i.p.y - a.y) * a.w;
  o.p = float4(px * k.x - 1.0, 1.0 - py * k.y, i.p.z, 1.0);
  o.c = float4(1.0, 1.0, 1.0, 1.0); o.t0 = i.t0; o.t1 = i.t1;
  return o;
}
