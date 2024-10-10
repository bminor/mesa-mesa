local r = execute {
  src=[[
    @id      g3

    mov(8)   g4<1>F    g3<1>UD                       {A@1};
    mov(8)   g5<1>F    g4.2<0,1,0>F                  {A@1};

    // Moving from F to BF (packed) doesn't work, but that's not much
    // of a problem because BFloat16 is a cropped version of Float32.
    //
    // So instead of
    //
    //     mov(8)   g10<1>BF  g4<1>F
    //
    // use MOV with UW and appropriate offset.
    mov(8)   g10<1>UW  g4.1<2>UW                     {A@1};

    mad(8)   g11<1>BF  g4<1>F    g10<1>BF  g5<1>F    {A@1};
    add(8)   g12<1>BF  g11<1>BF  g4<1>F              {A@1};

    // For similar reason as above, instead of
    //
    //     mov(8)   g20<1>F   g12<1>BF
    //
    // use a SHL unpacking into UD.
    shl(8)   g20<1>UD  g12<1>UW  16UW                {A@1};

    mov(8)   g21<1>UD  g20<1>F                       {A@1};
    @write   g3        g21

    @eot
  ]]
}

expected = {[0] = 0, 4, 8, 12, 16, 20, 24, 28}

print("result")
dump(r, 8)

print("expected")
dump(expected, 8)

for i=0,7 do
  if r[i] ~= expected[i] then
    print("FAIL")
    return
  end
end

print("OK")
