# glutplanet
Simple map viewer like SAS.Planet.<br>
View tiles from Bing,Yandex,OpenStreetMap and other.<br>
Store tiles local in TMS format.<br>

Used CURL for download tiles.<br>
Used stb_image for read jpeg, png tiles.<br>
And TinyCThread for download, load in separated thread.<br>

Little size One source file in C.<br>

TODO:<br>
fix first time draw.<br>
fix sometime error: corrupted double-linked list after free(tile).<br>
add blend.<br>
raplace tiles_loaded form flat array to ListTile or SyncTileList with sorted push
