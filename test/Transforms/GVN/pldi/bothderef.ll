declare void @g(i32*, i32*)
declare void @h()
declare void @hh(i32, i32)
define void @f(i32 %n, i32 %m) {
  %alc = alloca i32
  %alc2 = alloca i32
  %a = getelementptr inbounds i32, i32* %alc, i32 %n
  %b = getelementptr inbounds i32, i32* %alc2, i32 %m
  call void @g(i32* %a, i32* %b)
  %tmp1 = load i32, i32* %a
  %tmp2 = load i32, i32* %b
  %c= icmp eq i32* %a, %b
  br i1 %c, label %A, label %B
A:
  call void @g(i32* %a, i32* %b)
  call void @hh(i32 %tmp1, i32 %tmp2)
  ret void
B:
  call void @h()
  call void @hh(i32 %tmp1, i32 %tmp2)
  ret void
}
