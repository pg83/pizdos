#!/bin/sh
# Makes the corpus of the stand's decode work (dev/stand.py): files for the
# modules of impulse-desktop/decode, from free sources and by their real
# encoders, with a MANIFEST of where each comes from and how it was made.
#
#   ix run bin/convert bin/heif bin/jxl bin/webp bin/djvulibre bin/mupdf bin/curl \
#       -- sh dev/stand/corpus.sh decode-corpus-1.tgz
#
# Corpus v1's encoder toolchain is ix 5cea87b37c702d257c19fba698dea94aa4ce6dd2.
#
# Sources: NASA's image library (public domain in the US, NASA's media
# guidelines), documents of the US government (public domain), a book of
# 1900 digitized by Google on archive.org (public domain; Google asks for
# non-commercial use), drawings of Wikimedia Commons under their own
# licenses, and the SVG corpus of impulse-desktop/svg, each file under the
# license of the project it comes from.
set -eu

# ix run points SSL_CERT_FILE at nowhere: curl takes its own trust store
[ -f "${SSL_CERT_FILE:-}" ] || unset SSL_CERT_FILE
out=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
mkdir -p src tmp corpus/im corpus/pdf corpus/djvu corpus/svg
UA="pizdos-stand-corpus/1 (https://github.com/pizd0s/kernel)"
SVG=https://raw.githubusercontent.com/impulse-desktop/svg/4fff81eedb729d315ed31a10bde833514034865c/tst/corpus

# fetch NAME SHA256 URL
fetch() {
    curl -sfL --retry 3 -A "$UA" -o "src/$1" "$3"
    echo "$2  src/$1" | sha256sum -c --quiet
}

# note FILE SOURCE HOW
note() {
    printf '%s\t%s\t%s\n' "$1" "$2" "$3" >> corpus/MANIFEST
}

im() {
    note "im/$1" "$2" "$3"
}

# magick writes the time into a PNG unless told not to
PNG="-define png:exclude-chunks=date,time"

nasa() {
    echo "https://images-assets.nasa.gov/image/$1/$1~orig.jpg" | sed 's/ /%20/g'
}

printf 'file\tsource\thow\n' > corpus/MANIFEST

fetch earth.jpg 7db92899f82d69217b824a3394353a187dd0e181e8768fad14d74ac75024f727 "$(nasa s102e5114)"
fetch portrait.jpg 8381da88e7ea144bf2cfed6ecf3692ad50d58d54728265b310cf746555cbb4b2 "$(nasa S84-40242)"
fetch spirit.jpg 6f7291ed2b5114719793f4e1666b2ea31c892c43e61807dc01a3fc199330898b "$(nasa PIA16440)"
fetch opportunity.jpg 646c2f00d05d7c8fc246c572f2617d70579b38b03b4289bc135ba29ceb826986 "$(nasa PIA16559)"
fetch sls.jpg 554c49b33fd8a81bc242abc6175db4a0518a5a17ea3a84045eeead3f6d81e5d3 "$(nasa 'SLS_KSC_Artemis II Rollout 1172026_21')"
fetch aircraft.jpg 497df1b8a5bfe76d8093ac184f0bc23f8d0ade7c71d3b07f1a66a7cda0162472 "$(nasa EC76-6048)"
fetch er2.jpg 18bfba5f6c51b83700f3c93afee4872dbb820120e32803f43db1edc61f41c946 "$(nasa AFRC2026-0012-420)"
fetch constitution.pdf 7f29c4a79e310cc7bcee5f3fd07fb1367e463bd4530fa5d854ebdd4fd4402e57 https://www.govinfo.gov/content/pkg/CDOC-110hdoc50/pdf/CDOC-110hdoc50.pdf
fetch fips197.pdf 62c86eb567f13edb8f71826e985da870b04ef6381634f303cdb16e84d47becd1 https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.197-upd1.pdf
fetch sp800-38d.pdf d99f3921ccebca049e7522426553aba071dae14ec3d5b6041e8c111a6cb57bba https://nvlpubs.nist.gov/nistpubs/Legacy/SP/nistspecialpublication800-38d.pdf
fetch book.djvu 086918e9d5f6e5f9bd0dc255f875da4e47e330f680116bd6f38f0eb72f3e1397 https://archive.org/download/suiprincipiidir00alimgoog/suiprincipiidir00alimgoog.djvu
fetch book.pdf 7bf9cd40eaa4a7bb3f6e6af2ff27fba88baf68e232d73ffe5fc8c4d55361d912 https://archive.org/download/suiprincipiidir00alimgoog/suiprincipiidir00alimgoog.pdf
fetch tiger.svg 5211e169283f43ab8ad7ea7998d917d5fbb3c568ac85c1a0217e86792822684d https://commons.wikimedia.org/wiki/Special:FilePath/Ghostscript_Tiger.svg
fetch mexico.svg bcfebaa9d94827919e65c5414e4ee35a076f89520a1b57d75e0949e1ca81c3b9 https://commons.wikimedia.org/wiki/Special:FilePath/Flag_of_Mexico.svg
fetch brazil.svg 041479c6a955990d32c8e45b7283afcd62eacc3592eba9bbf0162299ad77ff01 https://commons.wikimedia.org/wiki/Special:FilePath/Flag_of_Brazil.svg
fetch world.svg 88ae6cbcbe054c00455d16899724b348a102f49ff059bf7027c94e63b040e3e7 https://commons.wikimedia.org/wiki/Special:FilePath/BlankMap-World.svg

# im: the photos in the formats of the im module, at several sizes

cp src/earth.jpg corpus/im/earth.jpg
im earth.jpg "NASA s102e5114" "as published"

for f in png gif bmp tga ppm pcx sgi; do
    magick src/earth.jpg $PNG corpus/im/earth.$f
    im earth.$f "NASA s102e5114" "magick"
done

cwebp -quiet -q 80 corpus/im/earth.png -o corpus/im/earth.webp
im earth.webp "NASA s102e5114" "cwebp -q 80"

magick src/portrait.jpg -resize 2048x2048 tmp/portrait.png
magick tmp/portrait.png -quality 90 corpus/im/portrait-2048.jpg
im portrait-2048.jpg "NASA S84-40242" "resized to 2048, quality 90"
magick tmp/portrait.png -quality 85 -interlace JPEG corpus/im/portrait-2048-prog.jpg
im portrait-2048-prog.jpg "NASA S84-40242" "progressive, quality 85"
magick tmp/portrait.png -resize 1024x1024 $PNG corpus/im/portrait-1024.png
im portrait-1024.png "NASA S84-40242" "resized to 1024"
heif-enc -A -q 60 -o corpus/im/portrait-2048.avif tmp/portrait.png > /dev/null
im portrait-2048.avif "NASA S84-40242" "heif-enc -A -q 60"
cjxl tmp/portrait.png corpus/im/portrait-2048.jxl -d 1 2> /dev/null
im portrait-2048.jxl "NASA S84-40242" "cjxl -d 1"

cp src/spirit.jpg corpus/im/spirit.jpg
im spirit.jpg "NASA PIA16440" "as published, 11174x3162"
magick src/spirit.jpg -resize 2048x2048 tmp/spirit.png
cwebp -quiet -q 80 tmp/spirit.png -o corpus/im/spirit-2048.webp
im spirit-2048.webp "NASA PIA16440" "cwebp -q 80"
magick tmp/spirit.png -quality 30 corpus/im/spirit-2048.jp2
im spirit-2048.jp2 "NASA PIA16440" "JPEG 2000, quality 30"

cp src/opportunity.jpg corpus/im/opportunity.jpg
im opportunity.jpg "NASA PIA16559" "as published, 7753x2005"
magick src/opportunity.jpg -resize 1600x1600 tmp/opportunity.png
magick tmp/opportunity.png -compress LZW corpus/im/opportunity-1600.tif
im opportunity-1600.tif "NASA PIA16559" "TIFF, LZW"
cwebp -quiet -lossless tmp/opportunity.png -o corpus/im/opportunity-1600-ll.webp
im opportunity-1600-ll.webp "NASA PIA16559" "cwebp -lossless"

magick src/sls.jpg -resize 2048x2048 tmp/sls.png
magick tmp/sls.png -quality 85 corpus/im/sls-2048.jpg
im sls-2048.jpg "NASA SLS Artemis II rollout" "quality 85"
heif-enc -A -q 60 -o corpus/im/sls-2048.avif tmp/sls.png > /dev/null
im sls-2048.avif "NASA SLS Artemis II rollout" "heif-enc -A -q 60"
magick tmp/sls.png -resize 1024x1024 tmp/sls-1024.png
cjxl tmp/sls-1024.png corpus/im/sls-1024-ll.jxl -d 0 2> /dev/null
im sls-1024-ll.jxl "NASA SLS Artemis II rollout" "cjxl -d 0, lossless"
magick tmp/sls-1024.png \( +clone -fx 'j / h' \) -alpha off -compose CopyOpacity -composite $PNG corpus/im/sls-1024-alpha.png
im sls-1024-alpha.png "NASA SLS Artemis II rollout" "alpha fading to the bottom"
cwebp -quiet -q 80 -exact corpus/im/sls-1024-alpha.png -o corpus/im/sls-1024-alpha.webp
im sls-1024-alpha.webp "NASA SLS Artemis II rollout" "cwebp -q 80, with alpha"
heif-enc -A -q 60 -o corpus/im/sls-1024-alpha.avif corpus/im/sls-1024-alpha.png > /dev/null
im sls-1024-alpha.avif "NASA SLS Artemis II rollout" "heif-enc -A -q 60, with alpha"

magick src/aircraft.jpg -resize 1600x1600 tmp/aircraft.png
magick tmp/aircraft.png -quality 85 corpus/im/aircraft-1600.jpg
im aircraft-1600.jpg "NASA EC76-6048" "quality 85"
magick tmp/aircraft.png -colors 256 corpus/im/aircraft-1600.gif
im aircraft-1600.gif "NASA EC76-6048" "256 colors"
magick tmp/aircraft.png -colorspace Gray $PNG corpus/im/aircraft-1600-gray.png
im aircraft-1600-gray.png "NASA EC76-6048" "gray"

magick src/er2.jpg -resize 2048x2048 tmp/er2.png
cjxl tmp/er2.png corpus/im/er2-2048.jxl -d 1.5 2> /dev/null
im er2-2048.jxl "NASA AFRC2026-0012-420" "cjxl -d 1.5"
magick tmp/er2.png -quality 60 corpus/im/er2-2048-q60.jpg
im er2-2048-q60.jpg "NASA AFRC2026-0012-420" "quality 60"
magick tmp/er2.png -resize 1600x1600 -depth 16 -compress Zip corpus/im/er2-1600-16bit.tif
im er2-1600-16bit.tif "NASA AFRC2026-0012-420" "TIFF, 16 bits, deflate"
magick tmp/er2.png -quality 25 corpus/im/er2-2048.jp2
im er2-2048.jp2 "NASA AFRC2026-0012-420" "JPEG 2000, quality 25"

mutool draw -q -w 2048 -o corpus/im/world-2048.png src/world.svg
im world-2048.png "Wikimedia Commons BlankMap-World.svg" "mutool draw -w 2048"
mutool draw -q -w 1600 -o corpus/im/tiger-1600.png src/tiger.svg
im tiger-1600.png "Wikimedia Commons Ghostscript_Tiger.svg" "mutool draw -w 1600"
magick -seed 7 -size 1024x768 plasma:fractal $PNG corpus/im/plasma-1024.png
im plasma-1024.png "made" "magick -seed 7 plasma:fractal"

# pdf: documents as published, and two made

cp src/constitution.pdf src/fips197.pdf src/sp800-38d.pdf src/book.pdf corpus/pdf/
note pdf/constitution.pdf "govinfo CDOC-110hdoc50" "as published"
note pdf/fips197.pdf "NIST FIPS 197 update 1" "as published"
note pdf/sp800-38d.pdf "NIST SP 800-38D" "as published"
note pdf/book.pdf "archive.org suiprincipiidir00alimgoog" "as published, a scan of 158 pages"
magick corpus/im/sls-2048.jpg corpus/im/aircraft-1600.jpg corpus/im/er2-2048-q60.jpg -compress jpeg -quality 85 corpus/pdf/photos.pdf
# and the time into a PDF: a date of the same length keeps the offsets
LC_ALL=C sed -i 's#/\(Creation\|Mod\)Date (D:[0-9]*)#/\1Date (D:20000101000000)#' corpus/pdf/photos.pdf
note pdf/photos.pdf "sls-2048.jpg, aircraft-1600.jpg, er2-2048-q60.jpg above" "magick, JPEG pages"
mutool convert -o corpus/pdf/vector.pdf src/tiger.svg src/mexico.svg src/world.svg
note pdf/vector.pdf "Wikimedia Commons Ghostscript_Tiger.svg, Flag_of_Mexico.svg, BlankMap-World.svg" "mutool convert"

# djvu: a scan as published, photos by c44, text pages by cjb2

cp src/book.djvu corpus/djvu/book.djvu
note djvu/book.djvu "archive.org suiprincipiidir00alimgoog" "as published, 158 pages"

for p in portrait sls er2 aircraft; do
    magick tmp/$p.png -resize 1600x1600 tmp/$p.ppm
    c44 -dpi 150 tmp/$p.ppm tmp/$p.djvu
done

djvm -c corpus/djvu/photos.djvu tmp/portrait.djvu tmp/sls.djvu tmp/er2.djvu tmp/aircraft.djvu
note djvu/photos.djvu "the photos above" "c44 -dpi 150 of each, djvm -c"
pages=""

for n in 1 2 3 4 5 6 7 8; do
    mutool draw -q -r 300 -c gray -o tmp/fips-$n.png src/fips197.pdf $n
    magick tmp/fips-$n.png -threshold 50% tmp/fips-$n.pbm
    cjb2 -dpi 300 tmp/fips-$n.pbm tmp/fips-$n.djvu
    pages="$pages tmp/fips-$n.djvu"
done

# shellcheck disable=SC2086
djvm -c corpus/djvu/text.djvu $pages
note djvu/text.djvu "NIST FIPS 197 update 1, pages 1-8" "mutool draw -r 300, threshold, cjb2 -dpi 300, djvm -c"

# svg: drawings of impulse-desktop/svg's corpus and of Wikimedia Commons

while read -r sha file; do
    name=$(echo "$file" | sed 's|/|--|')
    curl -sfL --retry 3 -o "corpus/svg/$name" "$SVG/$file"
    echo "$sha  corpus/svg/$name" | sha256sum -c --quiet
    note "svg/$name" "impulse-desktop/svg tst/corpus/$file" "as in the corpus"
done << 'SVGS'
ef330c72f6653699bb8167239b674dc21e5c2968052dc5501b061db9b9155d7c adwaita/adwaita-icon-theme-51.0__src__cursors__adwaita.svg
1bcc481a37cdde4099fe87b68660aed58c578719747100ad3a134d53012d6038 adwaita/adwaita-icon-theme-51.0__src__symbolic__core.svg
fac454f5179b2a659fa1d223cf310365ced9d29bde1fb11eb54c3f1824535023 batik/samples__asf-logo.svg
ea22936b9d44d93297563f255594ea03db64207d91b9101dfab8ccd425cedc67 batik/samples__mapSpain.svg
82a072e2c4abca518625907f52ad88514ea2000451b60871b964fc4f7cc6bb1b batik/samples__sunRise.svg
f6fd505730c7010ce7aa4199c9b985b1197d8d0919a1b83904c346630e69c9c0 diagrams/0f__0fa8dc46b06807b3.svg
f24971c7a0fe8f62db897d370872430e7992c9b010b9d23c0d2f97d1ba6bf52b diagrams/14__149d956b1ec8654a.svg
6821ceea7e08d4163329add8232a218a1f8a509d74fd85974e17d0492f4b6fa4 diagrams/cc__cc19c7f64d1037b9.svg
12935248051f786249a92e50e06d4d40579c8f3c2c9ed3bef8e3b8466c1b60f5 github/e4__e44a0b7fe229c31d.svg
0df9e2e40f1376f56c8cc164cba0be3dcf0f6129485dde52a5b26ec015d59564 github/e9__e996e249d240dd5b.svg
af23e9161a672057db89a6b0966026cad6ebd826136f7c68e468dd2f48d9e66a gnuplot/p0175.svg
406e8d8bf160f104bb53185166cbeb6938e14bfc7f4a807d04cb01cf6d917894 gnuplot/p0181.svg
d632f0dfd46283d411db644a43c0c7ef401b0d787792e6b867231cbd46ca00d9 graphviz/g0143-osage-cairo.svg
f4ed0897b856869aaeb47ca2535c34d7fe780acb356c15100d9ccf7218b6a6a7 graphviz/g0143-osage-svg.svg
7bc4c780356c3ccbb2733fcc7aa29705e7988d4d658647576e9c78683415e0bd inkscape/05cf8f0a7022dbb6.svg
21c338a16ec35e4ee6f77719cac3a6ca7eb1bf06116bd8d4e0df9d18ec4cc2a8 inkscape/35e6ee1bdc1b2618.svg
831e23c5213b175d61eec5ad750d32ef08f3df48392c91139669761998184d65 inkscape/dc4e3d4c2246aff8.svg
4571edfe8c4cc538914dc08b1a95013bde81c1b6328f3b56d395f3d39660201e librsvg/rsvg__tests__fixtures__reftests__svg1.1__filters-light-05-f.svg
a12cbdb09ae0d2e46247dde44bda61bedd0028b395e2fae0fb9b571f3b67c4b6 librsvg/rsvg__tests__fixtures__reftests__svg2__mix-blend-mode.svg
ab515991d9f4a54442b76b9e464ebfc76121bf686474353f6f6797a8a9c75e17 logos/lerna.svg
21f1bd868f59d55f2502e4138805808a6117a7869c5cbf27bbbc7957c06a0dc4 logos/matplotlib-icon.svg
fec20c5341b9117424a26c72a7e70a2ea645d0cb3a1d931c3820345741a696ba matplotlib/m0032.svg
24f6db6e248bca5bb70a41c8e7f702a8479a0223f7021bbe70685011a481d789 matplotlib/m0131.svg
294527a10a32548489fa2f824d2c2cea4a105e036ad86412ac404cb95e72913a notoemoji/third_party__region-flags__svg__MX-COL.svg
ed3e83e33b606ccc0eb05559cf5d5f7af01a47948b9150039109bd5966445015 notoemoji/third_party__region-flags__svg__TA.svg
e72e7700397e90cf6c4898a651e1511e30f07877e686b2c31eaab9082f93fb3c openclipart/286261.svg
b9f07fef9a2ef32f36f138325fa09b7de85a617d3e75c341bcfd305baa525a05 openclipart/356503.svg
fe2283f5fe1be9177dd49647232ec376b81783364dc74c50c4878be6b932418a poppler/t0054.svg
25535e1b7e5d619f98a098a1ff9155919d92bdf7a00f5a64582d5101ee7901a1 poppler/t0142.svg
f2efcf52cfb10a1fbe4ce70f6141c9b567f1238c43e349d7e16b982dceb2deb2 poppler/t0215.svg
34103ec832ce3aab8b96bc17633799769147752faf3e5905a454d62da2176e16 resvg/structure__image__embedded-16bit-png.svg
f1ba0778977328adeb5563840810d637806804895205be3a954457d0f2f9cbc4 resvg/structure__image__nested-embedded-png.svg
f60b9c05982070bbf27ec3727b76a8e2c6f321a166c401bbe088ffabb923372d sketchy-svgs/50__5037a6c649b84535.svg
8dc39a15a3aa63a9fb955c79a7db841daa2a6fe7523327d839c1c55d0d514914 sketchy-svgs/slow__0b__0b0743ef5fe3ff0f.svg
cb9a97a25dba2fdec288aa54dbf8e1ebf7b4025d00f0c855afe5e4b286d5b1be w3c/resources__Heb.svg
c6eb9911f762c146b716dedd2bafcfd8b062ac0b526e1a02438bc43b6e13d723 w3c/resources__SVGFreeSansBoldOblique-Lat1.svg
625bea3a458334e11588b52159ca77870a33687b80cfe14c7feda56fc1fe6a7f wikimedia/5d38502dc04b34d4.svg
c70320a4d494e4114f80b4a92de81b2f0e72038915bf4615ff876511be6a2787 wikimedia/f9da655f36d0db5e.svg
SVGS

cp src/tiger.svg src/mexico.svg src/brazil.svg corpus/svg/
note svg/tiger.svg "Wikimedia Commons Ghostscript_Tiger.svg" "as published"
note svg/mexico.svg "Wikimedia Commons Flag_of_Mexico.svg" "as published"
note svg/brazil.svg "Wikimedia Commons Flag_of_Brazil.svg" "as published"
gzip -9 -n -c src/tiger.svg > corpus/svg/tiger.svgz
note svg/tiger.svgz "Wikimedia Commons Ghostscript_Tiger.svg" "gzip -9"
gzip -9 -n -c src/world.svg > corpus/svg/world.svgz
note svg/world.svgz "Wikimedia Commons BlankMap-World.svg" "gzip -9"

python3 - corpus/im/earth.jpg corpus/im/tiger-1600.png corpus/svg/images.svg << 'PY'
import base64
import sys

jpeg, png, out = sys.argv[1:]


def url(path, kind):
    return f"data:image/{kind};base64," + base64.b64encode(open(path, "rb").read()).decode()


open(out, "w").write(f"""<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="1400" height="700">
<image x="0" y="0" width="700" height="462" xlink:href="{url(jpeg, 'jpeg')}"/>
<image x="700" y="0" width="700" height="700" xlink:href="{url(png, 'png')}" opacity="0.8"/>
<circle cx="700" cy="350" r="200" fill="none" stroke="#c03" stroke-width="12"/>
</svg>
""")
PY

note svg/images.svg "earth.jpg and tiger-1600.png above" "a JPEG and a PNG inside, as data URLs"

(cd corpus && find . -type f ! -name MANIFEST ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum) > corpus/SHA256SUMS
mv corpus decode-corpus-1
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -cf - decode-corpus-1 | gzip -9 -n > "$out"
ls -la "$out"
