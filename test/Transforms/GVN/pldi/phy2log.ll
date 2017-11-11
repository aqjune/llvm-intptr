; cannot propagate phy -> log
declare i32* @f()
define i32* @test() {
entry:
  %p = inttoptr i32 255 to i32*
  %q = call noalias i32* @f()
  %cmp = icmp eq i32* %p, %q
  br i1 %cmp, label %bb2, label %bb3
bb2:
  ret i32* %p
bb3:
  ret i32* %p
}

