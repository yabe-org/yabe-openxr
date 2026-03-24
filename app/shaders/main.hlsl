struct VertOutput {
  float4 pos: SV_POSITION;
};

VertOutput vert_main(float3 pos: POSITION) {
  VertOutput output;
  output.pos = float4(pos, 1.0);
  return output;
}

float4 frag_main(VertOutput input): SV_TARGET {
  return float4(0.0, 0.0, 0.4, 1.0);
  //return input.pos;
}

