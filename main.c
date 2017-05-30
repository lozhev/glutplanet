#include <GL/glut.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <curl/curl.h>
#include "tinycthread.h"
#include <search.h>
#if defined(WIN32)
#include <direct.h>
#endif
#define _USE_MATH_DEFINES
#include <math.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif


// Tile
typedef struct Tile {
	int z;
	int x;
	int y;
	GLuint tex;
	stbi_uc* texdata;
} Tile;

int cmp_tile(const void* l, const void* r) {
	const Tile* lsh = (const Tile*)l;
	const Tile* rsh = (const Tile*)r;
	if (lsh->z<rsh->z) return -1;
	if (lsh->z>rsh->z) return  1;
	if (lsh->x<rsh->x) return -1;
	if (lsh->x>rsh->x) return  1;
	if (lsh->y<rsh->y) return -1;
	if (lsh->y>rsh->y) return  1;
	return 0;
}

#define _mini(a,b) (a<b?a:b)
#define _maxi(a,b) (a>b?a:b)

double _mind(double a,double b) {
	return a<b?a:b;
}
double _maxd(double a,double b) {
	return a>b?a:b;
}
/*int _mini(int a,int b) {
	return a<b?a:b;
}
int _maxi(int a,int b) {
	return a>b?a:b;
}*/

// Coordinate
typedef struct {
	double row;
	double column;
	double zoom;
} crd_t;

void crd_print(const crd_t* crd) {
	fprintf(stderr,"crd %p, row: %f col:%f zoom: %f\n",crd,crd->row,crd->column,crd->zoom);
}

void crd_set(crd_t* crd, double row, double column) {
	crd->row = row;
	crd->column = column;
}

void crd_setz(crd_t* crd, double row, double column, double zoom) {
	crd->row = row;
	crd->column = column;
	crd->zoom = zoom;
}

void crd_set2(crd_t* crd, const crd_t* c, double point[2],double size[2]) {
	crd->row = c->row + (point[0] - size[0]*0.5)/256.0;
	crd->column = c->column + (point[1] - size[1]*0.5)/256.0;
	crd->zoom = c->zoom;
}

void crd_zoomby(crd_t* crd,double distance) {
	double adjust = pow(2, distance);
	crd->row *= adjust;
	crd->column *= adjust;
	crd->zoom += distance;
}
void crd_zoomto(crd_t* crd, double distance) {
	crd_zoomby(crd,distance - crd->zoom);
}

int crd_eq(crd_t* l, crd_t* r) {
	return l->row == r->row && l->column == r->column && l->zoom == r->zoom;
}

int crd_lt(crd_t* l, crd_t* r) {
	if (l->zoom == r->zoom) {
		if (l->row == r->row) {
			return l->column < r->column;
		}
		return l->row < r->row;
	}
	return l->zoom < r->zoom;
	//return l->zoom < r->zoom || \
	//(l->zoom == r->zoom && l->row < r->row) || \
	//(l->zoom == r->zoom && l->row == r->row && l->column < r->column);
}


// List
typedef struct TileNode {
	Tile* tile;
	struct TileNode* next;
	struct TileNode* prev;
} TileNode;

typedef struct TileList {
	size_t count;
	TileNode* first;
	TileNode* last;
} TileList;

typedef struct SyncTileList {
	size_t count;
	TileNode* first;
	TileNode* last;
	mtx_t mtx;
	cnd_t cnd;
} SyncTileList;

TileList* make_list() {
	//return (TileList*)calloc(1,sizeof(TileList));
	TileList* tl = (TileList*)malloc(sizeof(TileList));
	tl->count = 0;
	tl->first = 0;
	tl->last = 0;
	return tl;
}

SyncTileList* make_synclist() {
	SyncTileList* tl = (SyncTileList*)malloc(sizeof(SyncTileList));
	tl->count = 0;
	tl->first = 0;
	tl->last = 0;
	mtx_init(&tl->mtx,1);// 1 or 8??
	cnd_init(&tl->cnd);
	return tl;
}

void push_list(TileList* list,Tile* tile) {
	TileNode* node = (TileNode*)malloc(sizeof(TileNode));
	node->tile = tile;
	node->next=0;
	node->prev=0;
	if (list->last==0) {
		list->first = node;
		list->last = node;
	} else {
		list->last->next = node;
		node->prev = list->last;
		list->last = node;
	}
	++list->count;
}

void push_synclist(SyncTileList* list,Tile* tile) {
	TileNode* node = (TileNode*)malloc(sizeof(TileNode));
	node->tile = tile;
	node->next=0;
	node->prev=0;
	mtx_lock(&list->mtx); // call from main thread..
	if (list->last==0) {
		list->first = node;
		list->last = node;
	} else {
		list->last->next = node;
		node->prev = list->last;
		list->last = node;
	}
	++list->count;
	mtx_unlock(&list->mtx);
	cnd_signal(&list->cnd);
}

Tile* tile_wait_pull(SyncTileList* list){
	Tile* tile;
	TileNode* node;
	mtx_lock(&list->mtx);
	while(list->count==0){
		cnd_wait(&list->cnd,&list->mtx);
	}

	node = list->first;
	tile = node->tile;
	list->first = node->next;
	if (--list->count == 0) list->last=0;

	mtx_unlock(&list->mtx);
	free(node);
	return tile;
}

void pop_tile(TileList* list) {
	TileNode* node = list->first;
	Tile* tile = node->tile;
	list->first = node->next;
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);
	glDeleteTextures(1,&tile->tex);
	free(tile);
	free(node);
	--list->count;
}

/*int tile_cmp(const Tile* l, const Tile* r) {
	if (l->z < r->z) return -1;
	if (l->z > r->z) return  1;
	if (l->x < r->x) return -1;
	if (l->x > r->x) return  1;
	if (l->y < r->y) return -1;
	if (l->y > r->y) return  1;
	return 0;
}*/

Tile* find_tile(const TileList* list, Tile* tile) {
	const TileNode* cur = list->first;
	Tile* t;
	while (cur) {
		//if (tile_cmp(cur->tile,tile)==0) return cur->tile;
		t = cur->tile;
		if (t->z==tile->z&&t->x==tile->x&&t->y==tile->y) return t;
		cur = cur->next;
	}
	return 0;
}

void clear_list(const TileList* list) {
	const TileNode* n = list->first;
	while (n) {
		glDeleteTextures(1,&n->tile->tex);
		free(n->tile);
		n = n->next;
	}
}

void destroy_list(TileList* list) {
	TileNode* cur = list->first;
	while (cur) {
		if (cur->prev) free(cur->prev);
		cur = cur->next;
	}
	free(list->last);
	free(list);
}

void destroy_synclist(SyncTileList* list) {
	TileNode* cur = list->first;
	while (cur) {
		if (cur->prev) free(cur->prev);
		cur = cur->next;
	}
	free(list->last);
	free(list);
}

void print_tile(Tile* item) {
	fprintf(stderr,"Tile: %p z: %d x: %d y: %d\n",item,item->z,item->x,item->y);
}

void print_list(TileList* list) {
	TileNode* cur = list->first;
	while (cur) {
		print_tile(cur->tile);
		cur = cur->next;
	}
}

int clamp(int n, int lower, int upper) {
	return n < lower ? lower : n > upper ? upper : n;
}

// IO funcs
#ifndef _WIN32
#define _mkdir(path) mkdir(path, 0777)
#endif
int do_mkdir(const char* path) {
	struct stat st;
	int status = 0;

	if (stat(path, &st) != 0) {
		if (_mkdir(path) != 0 && errno != EEXIST)
			status = -1;
	} else if (!S_IFDIR&st.st_mode) {
		errno = ENOTDIR;
		status = -1;
	}

	return status;
}

int mkpath(const char* path) {
	char* pp, *sp;
	int status=0;
	char copypath[64];
	size_t slen = strlen(path);
	memcpy(copypath,path,slen);
	copypath[slen]='\0';
	pp = copypath;
	while (status == 0 && (sp = strchr(pp, '/')) != 0) {
		if (sp != pp) {
			*sp = '\0';
			status = do_mkdir(copypath);
			*sp = '/';
		}
		pp = sp + 1;
	}
	return status;
}

int exists(const char* name) {
	struct stat st;
	return stat(name, &st) == 0;
}

// MapProvider
typedef void(*MakeUrl)(void*,Tile*,char*);

typedef struct MapProvider {
	char* name;
	char* subdomians[4];
	char* urlformat;
	char* imgformat;
	MakeUrl makeurl;
} MapProvider;

// Global Vars
crd_t center;
int veiwport[2]= {800,600};

TileList* tiles;
SyncTileList* tiles_get;
Tile* tiles_draw[64];
int tiles_draw_count = 0;
Tile* tiles_loaded[256];
int tiles_loaded_count = 0;
MapProvider map;

void initMqcdnMap(MapProvider* map) {
	map->name = (char*)malloc(16);
	map->subdomians[0] = (char*)malloc(4);
	map->subdomians[1] = (char*)malloc(4);
	map->subdomians[2] = (char*)malloc(4);
	map->subdomians[3] = (char*)malloc(4);
	map->urlformat = (char*)malloc(128);
	map->imgformat = (char*)malloc(5);
	map->makeurl=0;

	sprintf(map->name,"mqcdn");
	sprintf(map->subdomians[0],"1");
	sprintf(map->subdomians[1],"2");
	sprintf(map->subdomians[2],"3");
	sprintf(map->subdomians[3],"4");
	sprintf(map->urlformat,"http://oatile%%s.mqcdn.com/tiles/1.0.0/sat/%%d/%%d/%%d.%%s");
	sprintf(map->imgformat,"jpg");
}

void initOSMMap(MapProvider* map) {
	map->name = (char*)malloc(16);
	map->subdomians[0] = (char*)malloc(4);
	map->subdomians[1] = (char*)malloc(4);
	map->subdomians[2] = (char*)malloc(4);
	map->subdomians[3] = (char*)malloc(4);
	map->urlformat = (char*)malloc(128);
	map->imgformat = (char*)malloc(4);
	map->makeurl=0;

	sprintf(map->name,"osm");
	sprintf(map->subdomians[0],"");
	sprintf(map->subdomians[1],"a.");
	sprintf(map->subdomians[2],"b.");
	sprintf(map->subdomians[3],"c.");
	sprintf(map->urlformat,"http://%%stile.openstreetmap.org/%%d/%%d/%%d.%%s");
	sprintf(map->imgformat,"png");
}

void getBindUrl(void* m,Tile* tile, char* url){
	MapProvider* map = (MapProvider*)m;
	int i = tile->z;
	char key[64]="";
	int r = rand()%4;
	for (; i > 0; --i) {
		int digit=0;
		char d[2];
		int mask = 1 << (i - 1);
		if (((int)tile->x & mask) != 0) ++digit;
		if (((int)tile->y & mask) != 0) digit+=2;
		sprintf(d,"%d",digit);
		strcat(key,d);
	}
	sprintf(url,"http://%s.tiles.virtualearth.net/tiles/a%s.jpeg?g=123",map->subdomians[r],key);
}
void initBingMap(MapProvider* map) {
	//zoom max 21
	map->name = (char*)malloc(16);
	map->subdomians[0] = (char*)malloc(4);
	map->subdomians[1] = (char*)malloc(4);
	map->subdomians[2] = (char*)malloc(4);
	map->subdomians[3] = (char*)malloc(4);
	map->urlformat = 0;
	map->imgformat = (char*)malloc(5);
	map->makeurl = getBindUrl;

	sprintf(map->name,"bing");
	sprintf(map->subdomians[0],"t0");
	sprintf(map->subdomians[1],"t1");
	sprintf(map->subdomians[2],"t2");
	sprintf(map->subdomians[3],"t3");
	sprintf(map->imgformat,"jpeg");
}

void getYahooUrl(void* map,Tile* tile, char* url){
	int x = tile->x;
	int y = ((1<<tile->z)-1) - tile->y - 1;
	int z = 18 - tile->z;
	sprintf(url,"http://us.maps3.yimg.com/aerial.maps.yimg.com/tile?v=1.7&t=a&x=%d&y=%d&z=%d",x,y,z);
	(void)map;
}

void initYahooMap(MapProvider* map) {
	map->name = (char*)malloc(16);
	map->subdomians[0] = (char*)malloc(4);
	map->subdomians[1] = (char*)malloc(4);
	map->subdomians[2] = (char*)malloc(4);
	map->subdomians[3] = (char*)malloc(4);
	map->urlformat = 0;
	map->imgformat = (char*)malloc(5);
	map->makeurl = getYahooUrl;

	sprintf(map->name,"yahoo");
	sprintf(map->subdomians[0],"t0");
	sprintf(map->subdomians[1],"t1");
	sprintf(map->subdomians[2],"t2");
	sprintf(map->subdomians[3],"t3");
	sprintf(map->imgformat,"png");
}

void initYndexMap(MapProvider* map) {
	map->name = (char*)malloc(16);
	map->subdomians[0] = (char*)malloc(2);
	map->subdomians[1] = (char*)malloc(2);
	map->subdomians[2] = (char*)malloc(2);
	map->subdomians[3] = (char*)malloc(2);
	map->urlformat = (char*)malloc(128);
	map->imgformat = (char*)malloc(6);
	map->makeurl = 0;

	sprintf(map->name,"yandex");
	sprintf(map->subdomians[0],"0");
	sprintf(map->subdomians[1],"1");
	sprintf(map->subdomians[2],"2");
	sprintf(map->subdomians[3],"3");
	//http://vec04.maps.yandex.net/tiles?l=map&v=2.16.0&x=9&y=6&z=4
	sprintf(map->urlformat,"http://vec0%%s.maps.yandex.net/tiles?l=map&v=2.26.0&z=%%d&x=%%d&y=%%d");
	sprintf(map->imgformat,"png");
	//https://static-maps.yandex.ru/1.x/
	//sprintf(map->subdomians[0],"");
	//sprintf(map->subdomians[1],"");
	//sprintf(map->subdomians[2],"");
	//sprintf(map->subdomians[3],"");
	//sprintf(map->urlformat,"https://static-maps%%s.yandex.ru/1.x/tiles?l=sat&z=%%d&x=%%d&y=%%d");
	//sprintf(map->imgformat,"jpg");
}

void destroyMap(MapProvider* map) {
	free(map->name);
	free(map->subdomians[0]);
	free(map->subdomians[1]);
	free(map->subdomians[2]);
	free(map->subdomians[3]);
	free(map->urlformat);
	free(map->imgformat);
}

void mapprovider_getFileName(MapProvider* map,Tile* tile,char* filename) {
	sprintf(filename,"%s/%d/%d/%d.%s",map->name,tile->z,tile->x,tile->y,map->imgformat);
}

void mapprovider_getUrlName(MapProvider* map,Tile* tile,char* url) {
	if (map->makeurl){
		map->makeurl(map,tile,url);
	} else {
		int r = rand()%4;
		sprintf(url,map->urlformat,map->subdomians[r],tile->z,tile->x,tile->y,map->imgformat);
	}
}

stbi_uc* getImageData(Tile* tile) {
	char filename[64];
	stbi_uc* data=0;
	int w,h,comp;
	mapprovider_getFileName(&map,tile,filename);
	if (exists(filename)) {
		data = stbi_load(filename,&w,&h,&comp,0);
	} else {
		CURL* curl = curl_easy_init();
		if (curl) {
			char url[128];
			FILE* stream=0;
			mapprovider_getUrlName(&map,tile,url);
			mkpath(filename);
			stream=fopen(filename, "wb");
			curl_easy_setopt(curl, CURLOPT_URL, url);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, stream);
			//fprintf(stderr,"download url %s\n",url);
			curl_easy_perform(curl);
			fclose(stream);

			data = stbi_load(filename,&w,&h,&comp,0);
			curl_easy_cleanup(curl);
		}
	}
	return data;
}
void idle(void);
int worker_thread(void* arg) {
	mtx_t mtx;
	mtx_init(&mtx,1);
	while (1) {
		Tile* tile = tile_wait_pull(tiles_get);
		stbi_uc* data = getImageData(tile);
		mtx_lock(&mtx);
		tile->texdata = data;
		tiles_loaded[tiles_loaded_count++] = tile;
		//glutPostRedisplay();
		mtx_unlock(&mtx);
	}
	return 0;
}

void getImageTile(Tile* tile) {
	tile->tex = 0;
	tile->texdata = 0;
	push_synclist(tiles_get,tile);
}
/*double round(double d){
	return floor(d + 0.5);
}*/
//int counter=0;
void make_tiles() {
	//int baseZoom = clamp(round(center.zoom), 0, 18);
	int baseZoom = clamp((int)floor(center.zoom+0.5), 0, 18);

	double tl[2]= {0,0};
	double tr[2]= {(double)veiwport[0],0};
	double bl[2]= {0,(double)veiwport[1]};
	double br[2]= {(double)veiwport[0],(double)veiwport[1]};

	int minCol;
	int maxCol;
	int minRow;
	int maxRow;
	int row_count;
	int col;

	crd_t ctl;
	crd_t ctr;
	crd_t cbl;
	crd_t cbr;
	crd_set2(&ctl,&center,tl,br);
	crd_zoomto(&ctl,baseZoom);
	crd_set2(&ctr,&center,tr,br);
	crd_zoomto(&ctr,baseZoom);
	crd_set2(&cbl,&center,bl,br);
	crd_zoomto(&cbl,baseZoom);
	crd_set2(&cbr,&center,br,br);
	crd_zoomto(&cbr,baseZoom);

	minCol = (int)floor(_mind(_mind(ctl.column,ctr.column),_mind(cbl.column,cbr.column)));
	maxCol = (int)floor(_maxd(_maxd(ctl.column,ctr.column),_maxd(cbl.column,cbr.column)));
	minRow = (int)floor(_mind(_mind(ctl.row,ctr.row),_mind(cbl.row,cbr.row)));
	maxRow = (int)floor(_maxd(_maxd(ctl.row,ctr.row),_maxd(cbl.row,cbr.row)));

	minCol -= 1;//FIXME: calc veiwport area
	maxCol += 1;

	row_count = (int)floor(pow(2.0, baseZoom))-1;
	minCol = _maxi(0,minCol);
	minRow = _maxi(0,minRow);
	maxCol = _mini(maxCol,row_count);
	maxRow = _mini(maxRow,row_count);

	tiles_draw_count = 0;
	col = minCol;
	for (; col <= maxCol; ++col) {
		int row = minRow;
		for (; row <= maxRow; ++row) {
			Tile tile = {baseZoom,col,row};
			//print_tile(&tile);
			Tile* ret = find_tile(tiles,&tile);
			if (ret==0) {
				Tile* newtile = (Tile*)malloc(sizeof(Tile));
				newtile->z = tile.z;
				newtile->x = tile.x;
				newtile->y = tile.y;
				getImageTile(newtile);
				push_list(tiles,newtile);
				//fprintf(stderr,"push tiles count: %ld\n",tiles->count);
				ret = newtile;

				if (tiles->count > 256){
					pop_tile(tiles);// TODO: remove tile in tiles_get
					//fprintf(stderr,"pop tiles count: %ld\n",tiles->count);
				}
				//++counter;
			}
			tiles_draw[tiles_draw_count++] = ret;
		}
	}

	//fprintf(stderr,"tiles count: %d tiles_draw_count: %d\n",counter,tiles_draw_count);
}

const double pi180= M_PI/180.0;
double getx(double lon,double zoom){
	return ((lon + 180) / 360) * pow(2, zoom);
}
double gety(double lat,double zoom){
	return (1 - log(tan(pi180*lat) + 1 / cos(pi180*lat)) / M_PI) /2 * pow(2, zoom);
}

double getxp(double lon,double pzoom){
	return ((lon + 180) / 360) * pzoom;
}
double getyp(double lat,double pzoom){
	return (1 - log(tan(pi180*lat) + 1 / cos(pi180*lat)) / M_PI) /2 * pzoom;
}

void draw(void) {
	int i;
	double tx;
	double ty;
	double powz;
	//bound srtm_04_01.tif
	double p[][2]={
		{-165.0,60.0},
		{-165.0+5.0,60.0},
		{-165.0+5.0,60.0-5.0},
		{-165.0,60.0-5.0}
	};
	double point[2];

	//idle();

	glClear(GL_COLOR_BUFFER_BIT);

	glColor3f( 1.0f, 1.0f, 1.0f );
	glEnable(GL_TEXTURE_2D);
	for (i=0; i<tiles_draw_count; ++i) {
		Tile* t = tiles_draw[i];

		double scale = pow(2.0, center.zoom - t->z);
		double ts = 256.0 * scale;
		crd_t coord = center;
		crd_zoomto(&coord,t->z);

		tx = (t->x - coord.column) * ts;
		ty = (t->y - coord.row) * ts;

		glBindTexture(GL_TEXTURE_2D, t->tex);
		glBegin(GL_TRIANGLE_STRIP);
		glVertex2d(tx, ty); glTexCoord2f(0, 1);
		glVertex2d(tx, ty+ts); glTexCoord2f(1, 0);
		glVertex2d(tx+ts, ty); glTexCoord2f(1, 1);
		glVertex2d(tx+ts, ty+ts); glTexCoord2f(0, 0);
		glEnd();
	}

	powz = pow(2, center.zoom);
	glColor3f( 1.0f, 0.0f, 0.0f );
	glDisable(GL_TEXTURE_2D);
	glBegin(GL_LINES);
	for (i = 0; i < 6000-1; ++i) {
		point[0] = getxp(p[0][0],powz);
		point[1] = getyp(p[0][1]-i*5.0/6000.0,powz);
		point[0] = 256.0 * (point[0] - center.column);
		point[1] = 256.0 * (point[1] - center.row);
		glVertex2d(point[0], point[1]);

		point[0] = getxp(p[1][0],powz);
		point[1] = getyp(p[0][1]-i*5.0/6000.0,powz);
		point[0] = 256.0 * (point[0] - center.column);
		point[1] = 256.0 * (point[1] - center.row);
		glVertex2d(point[0], point[1]);
	}
	for (i = 0; i < 6000-1; ++i) {
		point[0] = getxp(p[0][0]+i*5.0/6000.0,powz);
		point[1] = getyp(p[0][1],powz);
		point[0] = 256.0 * (point[0] - center.column);
		point[1] = 256.0 * (point[1] - center.row);
		glVertex2d(point[0], point[1]);

		point[0] = getxp(p[0][0]+i*5.0/6000.0,powz);
		point[1] = getyp(p[2][1],powz);
		point[0] = 256.0 * (point[0] - center.column);
		point[1] = 256.0 * (point[1] - center.row);
		glVertex2d(point[0], point[1]);
	}
	glEnd();

	glColor3f( 1.0f, 1.0f, 0.0f );
	glDisable(GL_TEXTURE_2D);
	glBegin(GL_LINE_STRIP);
	for (i = 0; i < 4; ++i) {
		point[0] = getxp(p[i][0],powz);
		point[1] = getyp(p[i][1],powz);
		point[0] = 256.0 * (point[0] - center.column);
		point[1] = 256.0 * (point[1] - center.row);
		glVertex2d(point[0], point[1]);
	}
	glEnd();

	glutSwapBuffers();
}

void reshape(int w, int h) {
	glViewport(0, 0, w, h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(-w/2, w/2, h/2, -h/2, -1, 1);
}

int moffsetx=0;
int moffsety=0;
int lastzoom=-1;
void mouse(int button, int state, int x, int y) {
	if (state == GLUT_DOWN) {
		moffsetx = x;
		moffsety = y;
	}

	if (button == 3) {
		// zoom:=12 for all earth??
		int zoom = (int)floor(center.zoom+0.5);
		crd_zoomby(&center,0.05);
		if(lastzoom!=zoom){
			lastzoom = zoom;
			//fprintf(stderr,"zoom:%d\n",lastzoom);
		}
		make_tiles();
		glutPostRedisplay();
	} else if (button == 4) {
		int zoom = (int)floor(center.zoom+0.5);
		crd_zoomby(&center,-0.05);
		if(lastzoom!=zoom){
			lastzoom = zoom;
			//fprintf(stderr,"zoom:%d\n",lastzoom);
		}
		make_tiles();
		glutPostRedisplay();
	}
}

void mousemove(int x,int y) {
	int ox = moffsetx - x;
	int oy = moffsety - y;
	moffsetx = x;
	moffsety = y;

	center.column += ox/256.0;
	center.row += oy/256.0;
	make_tiles();

	glutPostRedisplay();
}

void idle(void) {
	//struct timespec s;
	if (tiles_loaded_count>0) {
		Tile* tile = tiles_loaded[--tiles_loaded_count];
		GLuint textureId;
		glGenTextures(1, &textureId);
		glBindTexture(GL_TEXTURE_2D, textureId);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, tile->texdata);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
		//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		//printf("free texdata: %p\n",tile->texdata);
		free(tile->texdata);//FIXME: corrupted double-linked list
		tile->tex = textureId;
		glutPostRedisplay();
		return;
	}
	//s.tv_nsec = 50;
	//thrd_sleep(&s,0);
	Sleep(50);
}

double log2(double Value) {
	return log(Value) * (1.4426950408889634073599246810019);
}

void do_exit(){
	//destroy_synclist(tiles_get);
	clear_list(tiles);
	//destroy_list(tiles);
	//destroyMap(&map);
}

int main(int argc, char* argv[]) {
	thrd_t thrd1,thrd2,thrd3,thrd4;
	time_t tm;
	srand((unsigned int)time(&tm));
	curl_global_init(CURL_GLOBAL_WIN32/*CURL_GLOBAL_DEFAULT*/);// without ssl
	//initMqcdnMap(&map);
	//initOSMMap(&map);
	initBingMap(&map);
	//initYahooMap(&map);  //not work
	//initYndexMap(&map);

	glutInitWindowSize(veiwport[0], veiwport[1]);
	glutInit(&argc, argv);
	glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE);
	glutCreateWindow("glutplanet");
	glutMouseFunc(mouse);
	glutMotionFunc(mousemove);
	glutIdleFunc(idle);
	glutReshapeFunc(reshape);
	glutDisplayFunc(draw);

	tiles = make_list();
	tiles_get = make_synclist();
	thrd_create(&thrd1,worker_thread,0);
	thrd_create(&thrd2,worker_thread,0);
	thrd_create(&thrd3,worker_thread,0);
	thrd_create(&thrd4,worker_thread,0);
	crd_setz(&center,0.5,0.5,0);
	//double z = log2(veiwport[0]<veiwport[1]?veiwport[0]:veiwport[1] / 256.0);
	crd_zoomto(&center,log2(veiwport[0]<veiwport[1]?veiwport[0]:veiwport[1] / 256.0));
	make_tiles();

	atexit(do_exit);

	glutMainLoop();
	return 0;
}
