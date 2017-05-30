# glutplanet
Simple map viewer like SAS.Planet for windows, linux
View tiles from Bing,Yandex,OpenStreetMap and other.
Store tiles local in TMS format.

Used CURL for download tiles.
Used stb_image for read jpeg, png tiles.
And TinyCThread for download, load in separated thread.

Little size One source file in C

TODO:
fix first time draw.
fix sometime error: corrupted double-linked list after free(tile).
add blend.
raplace tiles_loaded form flat array to ListTile or SyncTileList with sorted push
