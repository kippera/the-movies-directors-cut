sampler2D s0 : register(s0);
float4 k : register(c0);   /* x=brightness y=contrast z=1/gamma w=saturation */
float4 p : register(c1);   /* x=1/width y=1/height z=sharpen(0..1) w=fxaa(0/1) */
float lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float4 main(float2 uv : TEXCOORD0) : COLOR {
  float2 t = p.xy;
  float3 m = tex2D(s0, uv).rgb;
  float3 c = m;
  float lM = lum(m);
  float lNW = lum(tex2D(s0, uv + float2(-t.x, -t.y)).rgb);
  float lNE = lum(tex2D(s0, uv + float2( t.x, -t.y)).rgb);
  float lSW = lum(tex2D(s0, uv + float2(-t.x,  t.y)).rgb);
  float lSE = lum(tex2D(s0, uv + float2( t.x,  t.y)).rgb);
  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
  float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
  /* FXAA (3.11 console style): blur along the local edge direction */
  float2 dir;
  dir.x = -((lNW + lNE) - (lSW + lSE));
  dir.y =  ((lNW + lSW) - (lNE + lSE));
  float dirReduce = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);
  float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
  dir = clamp(dir * rcp, -8.0, 8.0) * t;
  float3 rA = 0.5 * (tex2D(s0, uv + dir * (1.0/3.0 - 0.5)).rgb + tex2D(s0, uv + dir * (2.0/3.0 - 0.5)).rgb);
  float3 rB = rA * 0.5 + 0.25 * (tex2D(s0, uv + dir * -0.5).rgb + tex2D(s0, uv + dir * 0.5).rgb);
  float lB = lum(rB);
  float3 fx = (lB < lMin || lB > lMax) ? rA : rB;
  float edge = saturate((lMax - lMin - 0.0625) * 8.0);   /* leave flat areas untouched */
  c = lerp(m, lerp(m, fx, edge), p.w);
  /* sharpen: unsharp mask from the 4 diagonal neighbours */
  float3 nb = 0.25 * (tex2D(s0, uv + float2(-t.x, -t.y)).rgb + tex2D(s0, uv + float2(t.x, -t.y)).rgb + tex2D(s0, uv + float2(-t.x, t.y)).rgb + tex2D(s0, uv + float2(t.x, t.y)).rgb);
  c = c + (m - nb) * (p.z * 1.5);
  /* colour grading */
  c = (c - 0.5) * k.y + 0.5 + k.x;
  float l = lum(c);
  c = lerp(l.xxx, c, k.w);
  c = pow(saturate(c), k.z);
  return float4(c, 1);
}
