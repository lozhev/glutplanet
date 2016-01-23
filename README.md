# glutplanet
Simple map viewer like SAS.Planet
View tiles from Bing,Yandex,OpenStreetMap and other
Store local tiles in TMS format

Used CURL for download tiles
Used stb_image for read jpeg, png tiles
And TinyCThread for download, load in separated thread

Little size One source file in C

TODO:
fix sometime error: corrupted double-linked list after free(tile)
add blend
