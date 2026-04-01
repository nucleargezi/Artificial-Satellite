#import "@preview/zebraw:0.6.1": *

#let zebraw = zebraw.with(inset: (top: 3pt, bottom: 3pt))

#let text-fonts = (
  (name: "Latin Modern Roman", covers: "latin-in-cjk"),
  "Noto Serif CJK SC",
  "Noto Color Emoji",
)
#let code-fonts = (
  "Fira Code",
)

#let code-bg = rgb("#f5f7fb")


#let article-rules(body, lang: none, region: none) = {
  set page(
    width: 540pt,
    height: auto, // 不分页
    margin: (x: 24pt, y: 20pt),
  )

  show raw: set text(font: code-fonts, size: 6.7pt)
  show raw.where(block: false): it => box(
    fill: code-bg,
    inset: (x: 2pt, y: 1pt),
  )[
    #it
  ]
  show raw.where(block: true): it => zebraw(
    lang: false,
    background-color: code-bg,
    numbering: true,
    inset: (x: 10pt, y: 9pt), // x 援交半径 y 行间距
    it,
  )

  show math.equation.where(block: true): it => block(
    above: 1em,
    below: 1em,
  )[
    #align(center, it)
  ]

  body
}

#let shared-template(
  body,
) = {
  show: article-rules.with()
  body
}


#let main = shared-template.with()

#let mac-dots(size: 4pt, gap: 4pt) = stack(
  dir: ltr,
  spacing: gap,
  circle(radius: size, fill: rgb("#ff5f57"), stroke: none),
  circle(radius: size, fill: rgb("#febc2e"), stroke: none),
  circle(radius: size, fill: rgb("#28c840"), stroke: none),
)

#let snap-head(body) = {
  show: main.with()
  set page(
    foreground: place(
      top + left,
      dx: 15pt,
      dy: 12pt,
      mac-dots(),
    ),
    margin: (x: 10pt, y: 27pt),
    fill: code-bg,
  )
  body
}
