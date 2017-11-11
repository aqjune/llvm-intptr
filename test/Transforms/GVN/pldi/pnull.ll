declare void @g(i32*, i32*)

define void @nogvn_null(i32* %p) {
  %c = icmp eq i32* %p, null
  br i1 %c, label %lbl1, label %lbl2

lbl1:
  ; %p -> null is allowed.
  call void @g(i32* %p, i32* null)
  ret void
lbl2:
  ret void
}


