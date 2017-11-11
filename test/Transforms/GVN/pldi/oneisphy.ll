

define i32* @test(i32* %p, i8 %b) {
entry:
  %q = inttoptr i32 255 to i32*
  %cmp = icmp eq i32* %p, %q
  br i1 %cmp, label %bb2, label %bb3
bb2:
  ret i32* %q
bb3:
  ret i32* %q
}

