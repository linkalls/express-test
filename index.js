const express = require("express")
const app = express() //* 実行 オブジェクトにする
const port = 3000
// console.dir(app)

//* request,responseがcallbackで使える expressがjsのオブジェクトにparseしてくれる

// app.use((req, res) => {
//   //* オブジェクトのメソッド使う
//   console.log("リクエスト到達!!!!")
//   // console.dir(req)
//   // res.send("requestをうけたのでresponse返します")
//   //* responseを送ってくれる
//   // res.send({color: "red"})
//   res.send("<h1>hello world</h1>")
// }) //* どこのパスでもリクエストが来たら呼ばれる

app.get("/", (req, res) => {
  res.send("<h1>/にアクセスしました</h1>") //* res.sendは一行しか書けないからapp.useをオフにする
})

app.get("/dogs", (req, res) => {
  res.send("<h1>dogs</h1>")
})

app.post("/", (req, res) => {
  res.send("/に来た")
})

app.get("/r/:sub", (req, res) => {
  //* :subを取得するには reqオブジェクト内のparamsを使う
  // console.log(req.params)
  const { sub } = req.params //* 下と一緒ね
  // const sub = req.params.sub
  console.log(sub)
  res.send(`<h1>${sub}というsubredditです</h1>`)
})

app.get("/search", (req, res) => {
  // console.log(req.query)
  const { q } = req.query
  if (!q) {
    res.send(`<h1>検索するものがありません</h1>`)
  } else {
    res.send(`<h1>${q}の検索結果</h1>`)
  }
})

app.get("*", (req, res) => {
  //* *は全部 　下に置いてるのはrailsと一緒でうえからルーティング見るkら
  res.send("<h1>そんなのは知らねえ</h1>")
})

app.listen(port, () => {
  console.log("サーバーが起動したらここが実行される") //* コマンドが完了してない
})
