

define i32* @test(i8 %b) {
entry:
  %p = alloca i32, i32 4
  %q = getelementptr inbounds i32, i32* %p, i8 %b
  %cmp = icmp eq i32* %p, %q
  br i1 %cmp, label %bb2, label %bb3
bb2:
  ret i32* %p
bb3:
  ret i32* %p
}

