#include "glad/glad.h"
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
#include <process.h>
#include "dirent.h" // win32 only??
#endif
#define _USE_MATH_DEFINES
#include <math.h>

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

void print(const char* format, ...) {
	char buf[256];
	va_list argptr;
	va_start(argptr, format);
	vsprintf(buf, format, argptr);
	va_end(argptr);
	OutputDebugStringA(buf);
}

// Tile
typedef struct Tile {
	int z;            // level
	int x;
	int y;
	GLuint tex;       // texture
	GLuint ptex;      // parent texture
	stbi_uc* texdata; // image
	float vtx[16];    // vertices
	int del;          // delete texture
} Tile;

void tile_init(Tile* t,int x,int y,int z){
	t->x = x;
	t->y = y;
	t->z = z;
	t->tex = 0;
	t->ptex = 0;
	t->texdata = 0;
	t->del = 0;
}

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

int tile_parent(Tile* t, Tile* p) {
	//if(t->z == 0) return 0;

	p->z = t->z - 1;
	p->x = t->x / 2;
	p->y = t->y / 2;
	return 1;
}

int tile_quad(Tile* t) {
	int xeven,yeven;
	if(t->z == 0) return 0;

	xeven = (t->x & 1) == 0;
	yeven = (t->y & 1) == 0;
	return xeven && yeven ? 0 : xeven ? 2 : yeven ? 1 : 3;
}

int tile_make(Tile* t);
int tile_make_tex(Tile* t);


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

TileList* make_list() {
	//return (TileList*)calloc(1,sizeof(TileList));
	TileList* tl = (TileList*)malloc(sizeof(TileList));
	tl->count = 0;
	tl->first = 0;
	tl->last = 0;
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

void print_tile(Tile* item) {
	print("Tile: %p z: %d x: %d y: %d\n",item,item->z,item->x,item->y);
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

// Queue
typedef struct Node{
	char* data;
	struct Node* next, *prev;
}Node;

typedef struct{
	Node* first,*last;

	mtx_t mtx;
	cnd_t cnd;
	int count;
}Queue;

Queue* make_queue(){
	Queue *q = malloc(sizeof(Queue));
	q->first=0;
	q->last=0;
	q->count=0;
	mtx_init(&q->mtx, 1);
	cnd_init(&q->cnd);
	return q;
}

void queue_push(Queue* q,void* data){
	Node* n = (Node*)malloc(sizeof(Node));
	n->data = data;
	mtx_lock(&q->mtx);
	if (q->last){
		q->last->next = n;
	} else {
		q->first = n;
	}
	n->next = 0;
	q->last = n;
	++q->count;
	mtx_unlock(&q->mtx);
	cnd_signal(&q->cnd);
}

void* queue_pop(Queue* q){
	mtx_lock(&q->mtx);
	if (q->count){
		Node* n = q->first;
		void* ret = n->data;
		q->first = n->next;
		if (--q->count==0) q->last=0;
		mtx_unlock(&q->mtx);
		free(n);
		return ret;
	}
	mtx_unlock(&q->mtx);
	return 0;
}

void deque_push_back(Queue* q,void* data){
	Node* n = (Node*)malloc(sizeof(Node));
	n->data = data;
	mtx_lock(&q->mtx);
	if (q->count){
		q->last->next = n;
		n->prev = q->last;
		q->last = n;
	} else {
		q->last = n;
		q->first = n;
		n->prev = 0;
	}
	n->next = 0;
	++q->count;
	mtx_unlock(&q->mtx);
	cnd_signal(&q->cnd);
}

void deque_push_front(Queue* q,void* data){
	Node* n = (Node*)malloc(sizeof(Node));
	n->data = data;
	mtx_lock(&q->mtx);
	if (q->count){
		q->first->prev = n;
		n->next = q->first;
		q->first = n;
	} else {
		q->first = n;
		n->next = 0;
		q->last = n;
	}
	n->prev = 0;
	++q->count;
	mtx_unlock(&q->mtx);
	cnd_signal(&q->cnd);
}

void* deque_pop_back(Queue* q){
	Node* n;
	void* ret = 0;
	//mtx_lock(&q->mtx);
	n = q->last;
	if (n){
		ret = n->data;
		q->last = n->prev;
		if (--q->count==0) q->first=0;
		else q->last->next = 0;
		//print("q: %d\n",q->count);
		//mtx_unlock(&q->mtx);
		free(n);
		return ret;
	}
	//mtx_unlock(&q->mtx);
	return ret;
}

void* queue_pop_wait(Queue* q) {
	Node* n;
	void* ret = 0;
	mtx_lock(&q->mtx);
	while(q->first == 0) {
		cnd_wait(&q->cnd, &q->mtx);
	}

	n = q->first;
	ret = n->data;
	q->first = n->next;
	if(q->first == 0) q->last = 0;
	--q->count;
	//print("q: %d\n",q->count);
	mtx_unlock(&q->mtx);
	free(n);
	return ret;
}

Tile* tile_find(const Queue* q, Tile* tile) {
	const Node* cur = q->first;
	while (cur) {
		Tile* t = (Tile*)cur->data;
		if (t->z==tile->z&&t->x==tile->x&&t->y==tile->y) return t;
		cur = cur->next;
	}
	return 0;
}

void tile_delete(Queue* q, Tile* tile) {
	Node* cur = q->first;
	while (cur) {
		Tile* t = (Tile*)cur->data;
		if (t->z==tile->z&&t->x==tile->x&&t->y==tile->y) break;
		cur = cur->next;
	}
	if(!cur) return;
	//mtx_lock(&q->mtx);
	if(!cur->prev){
		q->first = cur->next;
	} if (!cur->next){
		cur->prev->next = 0;
		q->last = cur->prev;
	} else {
		cur->prev->next = cur->next;
	}
	--q->count;
	//mtx_unlock(&q->mtx);
	print("tiles_load %d\n",q->count);
	//print_tile((Tile*)cur->data);

	return;
}

void tile_tofirst(Queue* q, Tile* tile) {
	Node* cur;// = q->first;
	int has=0;
	mtx_lock(&q->mtx);
	if(q->count < 1) {
		mtx_unlock(&q->mtx);
		return;
	}
	//mtx_lock(&q->mtx);
	cur = q->first;
	while (cur) {
		Tile* t = (Tile*)cur->data;
		if (t->z==tile->z&&t->x==tile->x&&t->y==tile->y) { has=1; break; }
		cur = cur->next;
	}
	if(!has || q->first == cur) {
		mtx_unlock(&q->mtx);
		return;
	}
	//mtx_lock(&q->mtx);
	cur->prev->next = cur->next;
	if(cur->next)
		cur->next->prev = cur->prev;
	else
		q->last = cur->prev;

	q->first->prev = cur;

	cur->next = q->first;
	cur->prev = 0;
	q->first = cur;
	mtx_unlock(&q->mtx);
	return;
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

Queue* tiles;
Queue* tiles_load;
Queue* tiles_loaded;
Queue* tiles_delete;
Tile* tiles_draw[64];
int tiles_draw_count = 0;

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

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
	size_t written;
	written = fwrite(ptr, size, nmemb, stream);
	return written;
}

stbi_uc* getImageData(Tile* tile) {
	char filename[64];
	stbi_uc* data=0;
	int w,h,comp;
	mapprovider_getFileName(&map,tile,filename);
	if(exists(filename)) {
		data = stbi_load(filename, &w, &h, &comp, 0);
	} else {
		CURL* curl = curl_easy_init();
		if (curl) {
			char url[128];
			char tmp[L_tmpnam];
			FILE* stream=0;
			mapprovider_getUrlName(&map,tile,url);
			mkpath(filename);
			tmpnam(tmp);
			stream=fopen(tmp, "wb");
			curl_easy_setopt(curl, CURLOPT_URL, url);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, stream);
			//curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
			//print("download url %s\n",url);
			curl_easy_perform(curl);
			fclose(stream);
			rename(tmp,filename);

			data = stbi_load(filename,&w,&h,&comp,0);
			curl_easy_cleanup(curl);
		}
	}
	return data;
}

void to_draw(int z, int x, int y) {
	Tile tile = {z,x,y};
	Tile* ret = tile_find(tiles, &tile);
	//print_tile(&tile);
	if(ret == 0) {
		//int m;
		Tile* newtile = (Tile*)malloc(sizeof(Tile));
		tile_init(newtile, tile.x, tile.y, tile.z);
		tile_make(newtile);

		deque_push_front(tiles, newtile);
		deque_push_front(tiles_load, newtile);
		//fprintf(stderr,"push tiles count: %ld\n",tiles->count);
		tiles_draw[tiles_draw_count++] = newtile;

		if(tiles->count > 256) {
			Tile* t = deque_pop_back(tiles);
			t->del = 1;
			queue_push(tiles_delete,t);
			//tile_delete(tiles_load,t);
			//pop_tile(tiles);// TODO: remove tile in tiles_get
			//fprintf(stderr,"pop tiles count: %ld\n",tiles->count);
		}
	} else {
		//tile_tofirst(tiles_load,ret);
		tiles_draw[tiles_draw_count++] = ret;
	}
}

void make_tiles() {
	int baseZoom = clamp((int)floor(center.zoom+0.5), 0, 18);

	double tl[2]= {0,0};
	double tr[2]= {(double)veiwport[0],0};
	double bl[2]= {0,(double)veiwport[1]};
	double br[2]= {(double)veiwport[0],(double)veiwport[1]};

	int minCol, maxCol;
	int minRow, maxRow;
	int row_count;//, col;

	crd_t ctl, ctr;
	crd_t cbl, cbr;
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

	//print("area: %dx%d\n", (maxCol - minCol)+1, (maxRow - minRow)+1);

	tiles_draw_count = 0;
	/*col = minCol;
	for (; col <= maxCol; ++col) {
		int row = minRow;
		for (; row <= maxRow; ++row) {
			to_draw(baseZoom, col, row);
		}
	}*/
	/*{
		int SX=(maxCol - minCol)+1;
		int SY=(maxRow - minRow)+1;
		int min_x,min_y;
		int max_x,max_y;
		int j, dir=1;
		int x = (SX / 2)-!(SX & 1);
		int y = (SY / 2)-!(SY & 1);
		x += minCol;
		y += minRow;
		min_x = x; max_x = x;
		min_y = y; max_y = y;
		for(j=0;j<SX*SY;++j){
			to_draw(baseZoom,x,y);
			switch(dir){
			case 0:
				x-=1;
				if (x<min_x){ dir=1; min_x = x; }
				break;
			case 1:
				y+=1;
				if (y>max_y){ dir=2; max_y = y; }
				break;
			case 2:
				x+=1;
				if (x>max_x){ dir=3; max_x = x; }
				break;
			case 3:
				y-=1;
				if (y<min_y){ dir=0; min_y = y; }
				break;
			}
		}
	}*/
	{
		int j;
		int nx = maxCol;
		int ny = maxRow;
		int sx = minCol;
		int sy = minRow;
		int n = (nx-sx+1)*(ny-sy+1);
		while(n) {
			for(j = sy; j <= ny; ++j) {
				to_draw(baseZoom, nx, j); --n;
			} if(!n) break;
			nx--;
			for(j = nx; j >= sx; --j) {
				to_draw(baseZoom, j, ny); --n;
			} if(!n) break;
			ny--;
			for(j = ny; j >= sy; --j) {
				to_draw(baseZoom, sx, j); --n;
			} if(!n) break;
			sx++;
			for(j = sx; j <= nx; ++j) {
				to_draw(baseZoom, j, sy); --n;
			} if(!n) break;
			sy++;
		}
	}
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

#if _MSC_VER < 1900
double log2(double Value) {
	return log(Value) * (1.4426950408889634073599246810019);
}
#endif

void do_exit(void){
	//destroy_synclist(tiles_get);
//	clear_list(tiles);
	//destroy_list(tiles);
	//destroyMap(&map);
}

//#define TEST_QUEUE 1
#if TEST_QUEUE
typedef struct {
	float vtx[16];
	GLuint tex;
}Quad;

typedef struct{
	void* data;
	short w,h;
}TexData;

Queue path_list;
Queue tex_list;

TexData* loadImageData(char* filename) {
	stbi_uc* data;
	int w,h,comp;
	TexData* texdata;
	data = stbi_load(filename,&w,&h,&comp,0);
	if(data == 0) return 0;
#define S 256
	texdata = malloc(sizeof(TexData));
	if(w != S || h != S) {
		stbi_uc* out = malloc(S*S * 3);
		stbir_resize_uint8(data, w, h, 0, out, S, S, 0, comp);
		free(data);
		texdata->data = out;
		texdata->w = S;
		texdata->h = S;
	} else {
		texdata->data = data;
		texdata->w = w;
		texdata->h = h;
	}
#undef S
	//texdata->path = filename;
	//texdata->gltex = 0;
	return texdata;
}

#define _FOURCC(a, b, c, d) (a | (b << 8) | (c << 16) | (d << 24))

void worker_path(void* param){
	DIR *dir;
	dirent *ent;
	if ((dir = opendir(param)) != NULL){
		while ((ent = readdir(dir)) != NULL){
			if (ent->d_name[0]=='.') continue;
			if (S_ISDIR(ent->d_type)){
				char path[MAX_PATH];
				
				strcpy(path,param);
				strcat(path,ent->d_name);
				strcat(path,"/");
				// recursive
				worker_path(path);
			}
			else if (S_ISREG(ent->d_type)){
				int jpg[3] = {
					_FOURCC('.','j','p','g'),
					_FOURCC('.','J','P','G'),
					_FOURCC('j','p','e','g')
				};
				int ext = *(int*)&ent->d_name[ent->d_namlen-4];

				if (ext == jpg[1] || ext == jpg[0] || ext == jpg[2]){
					char* path = malloc(MAX_PATH);
					int len = strlen(param);
					strcpy(path,param);
					strcat(path,ent->d_name);
					//print("push %s\n",path);
					//queues_push(&path_list,path);
					queue_insert(&path_list,path);
				}
			}
		}
		closedir(dir);
	}
}

void worker_load(void* param){
	while(1){
		char* path = queue_pop_wait(&path_list);
		TexData* tdata = loadImageData(path);
		//print("load %s\n",path);
		if (tdata) queue_push(&tex_list,tdata);
		free(path);
	}
}

GLuint textures[4096];
int textures_count;
void make_textures(){
	//int i = 0;
	TexData* texd = queue_pop(&tex_list);
	while (texd /*&& i<4*/){
		GLuint textureId;
		glGenTextures(1, &textureId);
		glBindTexture(GL_TEXTURE_2D, textureId);
		glTexImage2D(GL_TEXTURE_2D, 0, 3, texd->w, texd->h, 0, GL_RGB, GL_UNSIGNED_BYTE, texd->data);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		//mtx_lock(&queue_mtx);
		free(texd->data);
		free(texd);
		//mtx_unlock(&queue_mtx);
		textures[textures_count++] = textureId;
		texd = queue_pop(&tex_list);
		//++i;
		//print("I: %d\n",i);
	}
}

void Render(float f){
	int i;
	double tx;
	double ty;

	make_textures();

	glClear(GL_COLOR_BUFFER_BIT);

	glColor3f( 1.0f, 1.0f, 1.0f );
	glEnable(GL_TEXTURE_2D);
	for (i=0; i<textures_count; ++i) {
		/*Tile* t = tiles_draw[i];

		double scale = pow(2.0, center.zoom - t->z);
		double ts = 256.0 * scale;
		crd_t coord = center;
		crd_zoomto(&coord,t->z);

		tx = (t->x - coord.column) * ts;
		ty = (t->y - coord.row) * ts;*/

		double scale = pow(2.0, center.zoom-4);
		double ts = 256 * scale;
		crd_t coord = center;
		crd_zoomto(&coord,4);
		tx = (i/10 -2- coord.column) * ts;
		ty = (i%10 -2- coord.row) * ts;

		glBindTexture(GL_TEXTURE_2D, textures[i]);
		glBegin(GL_TRIANGLE_STRIP);
		glVertex2d(tx, ty); glTexCoord2f(0, 1);
		glVertex2d(tx, ty+ts); glTexCoord2f(1, 0);
		glVertex2d(tx+ts, ty); glTexCoord2f(1, 1);
		glVertex2d(tx+ts, ty+ts); glTexCoord2f(0, 0);
		glEnd();
	}
	glutSwapBuffers();
}
#endif

const char vert_src[]=
"attribute vec4 a_pos;"
"uniform mat4 u_proj;"
"varying vec2 v_tex;"
"void main(){"
"	gl_Position = u_proj * vec4(a_pos.xy, 0, 1);"
"	v_tex = a_pos.zw;"
"}";

const char frag_src[]=
"uniform sampler2D u_tex;"
"varying vec2 v_tex;"
"void main(){"
"	gl_FragColor = texture2D(u_tex, v_tex);"
"}";

GLuint creatProg(const char* vert_src, const char* frag_src) {
	GLuint prog, vert_id, frag_id;

	vert_id = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vert_id, 1, &vert_src, 0);
	glCompileShader(vert_id);

	frag_id = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(frag_id, 1, &frag_src, 0);
	glCompileShader(frag_id);

	prog = glCreateProgram();
	glAttachShader(prog, vert_id);
	glAttachShader(prog, frag_id);
	glLinkProgram(prog);

	glDeleteShader(vert_id);
	glDeleteShader(frag_id);

	return prog;
}

GLuint loadGLTexture(int w, int h, void* data){
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	free(data);
	return tex;
}


int tile_make(Tile* t){
	float tx, ty;
	float* vtx = t->vtx;
	double scale = pow(2.0, center.zoom - t->z);
	float ts = (float)(256.0 * scale);
	crd_t coord = center;
	crd_zoomto(&coord,t->z);
	tx = (float)(t->x - coord.column) * ts;
	ty = (float)(t->y - coord.row) * ts;

	vtx[0] = tx;      vtx[1] = ty;
	vtx[4] = tx;      vtx[5] = ty + ts;
	vtx[8] = tx + ts; vtx[9] = ty;
	vtx[12]= tx + ts; vtx[13]= ty + ts;

	if(t->tex) {
		vtx[2] = 0; vtx[3] = 0;
		vtx[6] = 0; vtx[7] = 1;
		vtx[10]= 1; vtx[11]= 0;
		vtx[14]= 1; vtx[15]= 1;
	} else {
		int found = 0;
		float tz = .5f;
		int xoff = 0, yoff = 0;
		Tile r,*p;
		float fx1,fy1,fx2,fy2;
		int n = 2;

		tile_parent(t, &r);
		if(r.z == -1) return 0;
		xoff = t->x & 1;
		yoff = t->y & 1;
		p = tile_find(tiles,&r);
		if(p && p->tex) found = 1;
		while(!found) {
			xoff += (r.x & 1) * n;
			yoff += (r.y & 1) * n;
			tile_parent(&r, &r);
			if(r.z == -1) return 0;
			p = tile_find(tiles, &r);
			if(p && p->tex) found = 1;
			tz *= 0.5f;
			n *= 2;
			//if(p)tile_tofirst(tiles_load,p);
		};
		if (p==0) return 0;
		t->ptex = p->tex;
		tx = xoff * tz;
		ty = yoff * tz;
		fx1 = tx;
		fx2 = tx + tz;
		fy1 = ty;
		fy2 = ty + tz;
		vtx[2] = fx1; vtx[3] = fy1;
		vtx[6] = fx1; vtx[7] = fy2;
		vtx[10]= fx2; vtx[11]= fy1;
		vtx[14]= fx2; vtx[15]= fy2;
		return 1;
	}
	return 0;
}

void updateQuads() {
	int i = 0;
	for (i=0; i<tiles_draw_count; ++i) {
		Tile* t = tiles_draw[i];
		tile_make(t);
	}
}

GLuint prog;
GLuint u_proj;

void createOrthographicOffCenter(float left, float right, float bottom, float top,
								 float zNearPlane, float zFarPlane, float* dst) {
	int i;
	for (i = 1; i < 13; ++i) dst[i] = 0;
	dst[0] = 2 / (right - left);
	dst[5] = 2 / (top - bottom);
	dst[12] = (left + right) / (left - right);
	dst[10] = 1 / (zNearPlane - zFarPlane);
	dst[13] = (top + bottom) / (bottom - top);
	dst[14] = zNearPlane / (zNearPlane - zFarPlane);
	dst[15] = 1;
}

void reshape(int w, int h) {
	float m[16];
	glViewport(0, 0, w, h);
	if (prog){
		createOrthographicOffCenter(-w/2.f, w/2.f, h/2.f, -h/2.f, -1, 1, m);
		glUseProgram(prog);
		glUniformMatrix4fv(u_proj, 1, GL_FALSE, m);
	}
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
			//make_tiles();
		}
		make_tiles();
		updateQuads();
	} else if (button == 4) {
		int zoom = (int)floor(center.zoom+0.5);
		crd_zoomby(&center,-0.05);
		if(lastzoom!=zoom){
			lastzoom = zoom;
			//fprintf(stderr,"zoom:%d\n",lastzoom);
			//make_tiles();
		}
		make_tiles();
		updateQuads();
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
	updateQuads();
}

int tile_make_tex(Tile* t){
	GLuint textureId;
	float* vtx = t->vtx;
	glGenTextures(1, &textureId);
	glBindTexture(GL_TEXTURE_2D, textureId);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, t->texdata);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	free(t->texdata);
	t->texdata = 0;
	t->tex = textureId;
	//if(t->del == 1)print("can del\n");
	//if(t->del == 2)print("can del 2\n");
	vtx[2] = 0; vtx[3] = 0;
	vtx[6] = 0; vtx[7] = 1;
	vtx[10]= 1; vtx[11]= 0;
	vtx[14]= 1; vtx[15]= 1;
	return 1;
}

void worker_load(void* param){
	while(1){
		Tile* t = queue_pop_wait(tiles_load);
		if(t->del==0){
			t->texdata = getImageData(t);
			if (t->del==0 && t->texdata){
				queue_push(tiles_loaded,t);
			} else {
				//print("delete after load %p\n",t);
				free(t->texdata);
				free(t);
				//queue_push(tiles_delete,t);
			}
		} else {
			//print("delete %p\n",t);
			free(t);
			//queue_push(tiles_delete,t);
		}
		//t->del = 2;
	}
}
int ltwo = -1;
void Render(float f){
	int i = 0, ch = 0, one = 0, two = 0;;
	GLuint ltex=-1;
	Tile* t;
	Node* n = tiles_delete->first;

	if(n) {
		t = (Tile*)n->data;
		if(t->del == 1) {
			one++;
			glDeleteTextures(1, &t->tex);
			tiles_delete->first = n->next;
			free(n);
			tiles_delete->count--;
			if(tiles_delete->count == 0) tiles_delete->last = 0;
			//print("deleted from tiles_delete count: %d\n", tiles_delete->count);
		}
		if(t->del == 2)two++;
		//n = n->next;
	}
	//if(one) print("--------------- ONE --------------- %d\n",one);
	if(ltwo != two) {
		//print("--------------- TWO --------------- %d\n", two);
		ltwo = two;
	}

	/*Tile* t = queue_pop(tiles_delete);
	while (t /*&& i<4* /){
		if (t->tex)
			glDeleteTextures(1,&t->tex);
		free(t);
		t = queue_pop(tiles_delete);
		i++;
	}
	if (i) print("deleted %d\n",i);*/

	i=0;
	t = queue_pop(tiles_loaded);
	while (t && i<10){
		tile_make_tex(t);
		t = queue_pop(tiles_loaded);
		ch=1;
		i++;
	}
	//if (i) print("loaded %d\n",i);
	if (ch){
		make_tiles();
		updateQuads();
	}


	glClear(GL_COLOR_BUFFER_BIT);

//	glColor3f( 1.0f, 1.0f, 1.0f );
	glEnable(GL_TEXTURE_2D);
	glEnableVertexAttribArray(0);
	for (i=0; i<tiles_draw_count; ++i) {
		Tile* t = tiles_draw[i];

		if (t->tex){
			glBindTexture(GL_TEXTURE_2D,t->tex);
			ltex = t->tex;
		} else {
			if (ltex != t->ptex){
				glBindTexture(GL_TEXTURE_2D,t->ptex);
				ltex = t->ptex;
			}
		}

		glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,0,t->vtx);
		glDrawArrays(GL_TRIANGLE_STRIP,0,4);
	}
}

void Draw_empty(void){}

int num_cores(){
#if _WIN32
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	return sysinfo.dwNumberOfProcessors;
#elif __linux
	return sysconf(_SC_NPROCESSORS_ONLN);
#else
#error "platform error"
#endif
} 

int main(int argc, char* argv[]) {
	int i;
//	thrd_t thrd1,thrd2,thrd3,thrd4;
	time_t tm;
	srand((unsigned int)time(&tm));

	glutInitWindowSize(veiwport[0], veiwport[1]);
	glutInit(&argc, argv);
	glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE);
	glutCreateWindow("glutplanet");
	glutMouseFunc(mouse);
	glutMotionFunc(mousemove);
	//glutIdleFunc(idle);
	glutReshapeFunc(reshape);
	glutDisplayFunc(Draw_empty/*draw*/);

#if TEST_QUEUE
	{int n;
	queue_init(&path_list);
	queue_init(&tex_list);

	//CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)worker_path, "d:/libs/", 0, NULL);
	_beginthread(worker_path,0,"d:/libs/");
	n = num_cores();
	for (;n;n--)
		_beginthread(worker_load,0,0);}
#else
	curl_global_init(CURL_GLOBAL_WIN32);

	//initMqcdnMap(&map);
	//initOSMMap(&map);
	initBingMap(&map);
	//initYahooMap(&map);  //not work
	//initYndexMap(&map);
	gladLoadGL();
	prog = creatProg(vert_src,frag_src);
	u_proj = glGetUniformLocation(prog, "u_proj");
	crd_setz(&center,0.5,0.5,0);
	crd_zoomto(&center,log2(veiwport[0]<veiwport[1]?veiwport[0]:veiwport[1] / 256.0));
	lastzoom = (int)floor(center.zoom+0.5);

	tiles = make_queue();
	tiles_load = make_queue();
	tiles_loaded = make_queue();
	tiles_delete = make_queue();
	i = 8;// num_cores();
	while(i--) _beginthread(worker_load, 0, 0);
	
	make_tiles();
	
#endif

	for (;;){
		glutMainLoopEvent();

		Render(0);
		glutSwapBuffers();
	}

	return 0;
}
