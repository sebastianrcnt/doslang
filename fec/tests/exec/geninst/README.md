# 제네릭 인스턴스 리터럴

`Name(args){...}` 와 `binding.Name(args){...}` 로 제네릭의 인스턴스를 짓는다.
전에는 `Self{...}` 나 생성자 함수로만 만들 수 있었다.

`Handle(T)` 는 `T` 를 본문에서 쓰지 않는다 -- typed handle 의 자연스러운 모양이고,
`Handle(Node)` 와 `Handle(Kind)` 를 갈라놓는 것 말고는 하는 일이 없다. 그 둘이
실제로 다른 타입이라는 것은 `generic/badphant.fe` 가 거부로 고정한다.
