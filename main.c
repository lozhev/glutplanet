#include "glad/glad.h"
#include <GL/freeglut_std.h>
#include <GL/freeglut_ext.h> // glutMainLoopEvent
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <curl/curl.h>
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


#if _WIN32
#define StartThread(start,arg) CreateThread(NULL, 0, start, (void*)arg, 0, NULL)
typedef CRITICAL_SECTION mtx_t;
#define mtx_init(m) InitializeCriticalSection(m)
#define mtx_destroy(m) DeleteCriticalSection(m)
#define mtx_lock(m) EnterCriticalSection(m)
#define mtx_unlock(m) LeaveCriticalSection(m)
#if !defined(_WIN32_WINNT) || !defined(_WIN32_WINNT_VISTA) || (_WIN32_WINNT < _WIN32_WINNT_VISTA)
typedef HANDLE cnd_t;
#define cnd_init(c) *c = CreateEvent(NULL, FALSE, FALSE, NULL); 
#define cnd_destroy(c) CloseHandle(*c)
#define cnd_signal(c) SetEvent(*c)
#define cnd_wait(c,m) mtx_unlock(m); WaitForSingleObject(*c,INFINITE); mtx_lock(m)
#else
typedef CONDITION_VARIABLE cnd_t;
#define cnd_init(c) InitializeConditionVariable(c)
#define cnd_destroy(c)
#define cnd_signal(c) WakeConditionVariable(c)
#define cnd_wait(c,m) SleepConditionVariableCS(c,m,INFINITE)
#endif
void print(const char* format, ...) {
	char buf[256];
	va_list argptr;
	va_start(argptr, format);
	vsprintf(buf, format, argptr);
	va_end(argptr);
	OutputDebugStringA(buf);
}
#elif __linux
#include <pthread.h>
#include <unistd.h>
#define StartThread(start,arg) {\
pthread_t th;\
pthread_create(&th, 0, start, (void*)arg);\
}
typedef pthread_mutex_t mtx_t;
#define mtx_init(m) pthread_mutex_init(m, 0)
#define mtx_destroy(m) pthread_mutex_destroy(m)
#define mtx_lock(m) pthread_mutex_lock(m)
#define mtx_unlock(m) pthread_mutex_unlock(m)
typedef pthread_cond_t cnd_t;
#define cnd_init(c) pthread_cond_init(c, 0)
#define cnd_destroy(c) pthread_cond_destroy(c)
#define cnd_signal(c) pthread_cond_signal(c)
#define cnd_wait(c,m) pthread_cond_wait(c,m)
void print(const char* format, ...) {
	va_list argptr;
	va_start(argptr, format);
	vfprintf(stderr, format, argptr);
	va_end(argptr);
}
#endif

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
	volatile int ref;
} Tile;

void tile_init(Tile* t,int x,int y,int z){
	t->x = x;
	t->y = y;
	t->z = z;
	t->tex = 0;
	t->ptex = 0;
	t->texdata = 0;
	t->del = 0;
	t->ref = 1;
}

/*int tile_release(Tile* t){
	--t->ref;
	if (t->ref==0){
		if (t->texdata) {
			free(t->texdata);
			t->texdata=0;
			//print("release texdata\n");
		}
		if(t->tex){
			glDeleteTextures(1,&t->tex);
			t->tex = 0;
		}
		free(t);
		return 1;
	}
	return 0;
}*/

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


#define mini(a,b) (a<b?a:b)
#define maxi(a,b) (a>b?a:b)

double _mind(double a,double b) {
	return a<b?a:b;
}
double _maxd(double a,double b) {
	return a>b?a:b;
}

#define mind(a,b) (a<b?a:b)
#define maxd(a,b) (a>b?a:b)


// Coordinate
typedef struct {
	union { double row; double x; };
	union { double column; double y; };
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
	Node* first, *last;

	mtx_t mtx;
	cnd_t cnd;
	int count;
}Queue;

Queue* make_queue(){
	Queue *q = malloc(sizeof(Queue));
	q->first=0;
	q->last=0;
	q->count=0;
	mtx_init(&q->mtx);
	cnd_init(&q->cnd);
	return q;
}
//TODO: preallocated nodes
void queue_push(Queue* q,void* data){
	Node* n = (Node*)malloc(sizeof(Node));
	n->data = data;
	if (q->last){
		q->last->next = n;
	} else {
		q->first = n;
	}
	n->next = 0;
	q->last = n;
	++q->count;
}

void queue_push_s(Queue* q,void* data){
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
	if (q->count){
		Node* n = q->first;
		void* ret = n->data;
		q->first = n->next;
		if (--q->count==0) q->last=0;
		free(n);
		return ret;
	}
	return 0;
}

void* queue_pop_s(Queue* q){
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
}

void deque_push_front_s(Queue* q,void* data){
	Node* n = (Node*)malloc(sizeof(Node));
	n->data = data;
	mtx_lock(&q->mtx);
	if (q->count){
		q->first->prev = n;
		n->next = q->first;
		q->first = n;
	} else {
		q->first = n;
		q->last = n;
		n->next = 0;
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

void* deque_pop_back_s(Queue* q) {
	mtx_lock(&q->mtx);
	if(q->last) {
		Node* n = q->last;
		void* ret = n->data;
		q->last = n->prev;
		if(--q->count == 0) q->first = 0;
		else q->last->next = 0;
		//print("q: %d\n",q->count);
		mtx_unlock(&q->mtx);
		free(n);
		return ret;
	}
	mtx_unlock(&q->mtx);
	return 0;
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

typedef struct {
	void** data;
	void* end;
	int count;
	int cap;
}Array;

Array* make_array(int count) {
	Array* a = (Array*)malloc(sizeof(Array));
	a->data = malloc(count * sizeof(void*));
	a->count = 0;
	a->cap = count;
	return a;
}
mtx_t g_mtx;
void array_push(Array* a, void* data) {
	if(a->count == a->cap) {
		int i;
		void** data;
		a->cap += 20;
		print("realloc: %3d %p count: %d\n", a->cap, a, a->count);
		//mtx_lock(&g_mtx);
		//a->data = realloc(a->data, a->cap * sizeof(void*));
		data = malloc(a->cap * sizeof(void*));
		//mtx_unlock(&g_mtx);
		for(i = 0; i < a->count; ++i) {
			data[i] = a->data[i];
			//print("%d %p\n", i, t);// print_tile(t);
		}
		free(a->data);
		a->data = data;
	}
	a->data[a->count++] = data;
}

void* array_pop(Array* a) {
	if (a->count) return a->data[--a->count];
	return 0;
}

// most hot function..
Tile* tile_find(const Queue* q, Tile* tile) {
	const Node* cur = q->first;
	while (cur) {
		Tile* t = (Tile*)cur->data;
		if (t->z==tile->z&&t->x==tile->x&&t->y==tile->y) return t;
		cur = cur->next;
	}
	return 0;
}

/*void tile_delete(Queue* q, Tile* tile) {
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
	free(cur);
	--q->count;
	//mtx_unlock(&q->mtx);
	print("tiles_load %d\n",q->count);
	//print_tile((Tile*)cur->data);

	return;
}*/

void tile_tofirst(Queue* q, Tile* tile) {
	Node* cur;// = q->first;
	int has=0;
	if(q->count < 1) return;
	
	cur = q->first;
	while (cur) {
		Tile* t = (Tile*)cur->data;
		if (t->z==tile->z&&t->x==tile->x&&t->y==tile->y) { has=1; break; }
		cur = cur->next;
	}
	if(!has || q->first == cur) return;

	cur->prev->next = cur->next;
	if(cur->next)
		cur->next->prev = cur->prev;
	else
		q->last = cur->prev;

	q->first->prev = cur;

	cur->next = q->first;
	cur->prev = 0;
	q->first = cur;
	return;
}

void tile_tofirst_s(Queue* q, Tile* tile) {
	Node* cur;// = q->first;
	int has=0;
	mtx_lock(&q->mtx);
	if(q->count < 1) {
		mtx_unlock(&q->mtx);
		return;
	}

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
	char name[16];
	char subdomians[4][4];
	char urlformat[128];
	char imgformat[5];
	MakeUrl makeurl;
} MapProvider;

// Global Vars
crd_t center;
int veiwport[2]= {800,600};

Queue* tiles;
Queue* tiles_load;
//Queue* tiles_loaded;
Array* tiles_loaded;
//Queue* tiles_release;
Array* tiles_release;
Tile* tiles_draw[64];
int tiles_draw_count = 0;

int tile_release(Tile* t) {
	--t->ref;
	if(t->ref == 0) {
		array_push(tiles_release, t);
		/*if(t->texdata) {
			free(t->texdata);
			t->texdata = 0;
			//print("release texdata\n");
		}
		if(t->tex) {
			glDeleteTextures(1, &t->tex);
			t->tex = 0;
		}
		free(t);*/
		return 1;
	}
	return 0;
}

MapProvider map;

void initMqcdnMap(MapProvider* map) {
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
		sprintf(d, "%d", digit); //itoa(digit, d, 10);
		strcat(key,d);
	}
	sprintf(url,"http://%s.tiles.virtualearth.net/tiles/a%s.jpeg?g=123",map->subdomians[r],key);
}
void initBingMap(MapProvider* map) {
	//zoom max 21
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
	map->makeurl = getYahooUrl;

	sprintf(map->name,"yahoo");
	sprintf(map->subdomians[0],"t0");
	sprintf(map->subdomians[1],"t1");
	sprintf(map->subdomians[2],"t2");
	sprintf(map->subdomians[3],"t3");
	sprintf(map->imgformat,"png");
}

void initYndexMap(MapProvider* map) {
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
	/*free(map->name);
	free(map->subdomians[0]);
	free(map->subdomians[1]);
	free(map->subdomians[2]);
	free(map->subdomians[3]);
	free(map->urlformat);
	free(map->imgformat);*/
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
			char tmp[64];
			FILE* stream=0;
			mapprovider_getUrlName(&map,tile,url);
			mkpath(filename);
			//tmpnam(tmp);
			strcpy(tmp,filename);
			strcat(tmp,".tmp");
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

//int last_r_count = -1;
//int release_count;

void tiles_limit() {
	if(tiles->count > 512) {// 4*6*18=432. 512 tiles ~100mb texures
		Tile* t = deque_pop_back(tiles);
		if(t->z == 1) deque_push_front(tiles, t); // keep top tiles
		else tile_release(t);
	}
	if(tiles_load->count > 512) {// free last from load safe??
		Tile* t = deque_pop_back_s(tiles_load);
		if(t->z == 1) deque_push_front_s(tiles_load, t); // keep top tiles
		else tile_release(t);
	}
}

void to_draw(int z, int x, int y) {
	Tile tile = {z,x,y};
	Tile* ret = tile_find(tiles, &tile);

	if(ret == 0) {
		Tile* newtile = (Tile*)malloc(sizeof(Tile));
		tile_init(newtile, tile.x, tile.y, tile.z);
		tile_make(newtile);

		newtile->ref+=2;
		deque_push_front(tiles, newtile);
		deque_push_front_s(tiles_load, newtile);

		//newtile->ref++;
		tiles_draw[tiles_draw_count++] = newtile;

		tiles_limit();

		tile_release(newtile);
	} else {
		tile_tofirst_s(tiles_load,ret);
		tile_tofirst(tiles,ret); // FIXME:!! second search
		//ret->ref++;
		tiles_draw[tiles_draw_count++] = ret;
	}
}

void make_tiles() {
	int j;
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

	minCol = (int)floor(_mind(mind(ctl.column,ctr.column),mind(cbl.column,cbr.column)));
	maxCol = (int)floor(_maxd(maxd(ctl.column,ctr.column),maxd(cbl.column,cbr.column)));
	minRow = (int)floor(_mind(mind(ctl.row,ctr.row),mind(cbl.row,cbr.row)));
	maxRow = (int)floor(_maxd(maxd(ctl.row,ctr.row),maxd(cbl.row,cbr.row)));

	minCol -= 1;//FIXME: calc veiwport area
	maxCol += 1;

	row_count = (int)floor(pow(2.0, baseZoom))-1;
	minCol = maxi(0,minCol);
	minRow = maxi(0,minRow);
	maxCol = mini(maxCol,row_count);
	maxRow = mini(maxRow,row_count);

	//print("area: %dx%d\n", (maxCol - minCol)+1, (maxRow - minRow)+1);

	/*for(j = tiles_draw_count - 1; j>=0; --j) {
		if (tile_release(tiles_draw[j])) print("tiles_draw release\n");
	}*/
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
		//int j;
		int nx = maxCol;
		int ny = maxRow;
		int sx = minCol;
		int sy = minRow;
		int n = (nx-sx+1)*(ny-sy+1);
		int sn = n;
		//
		Tile t[128],p;
		int t_count=0;
		Node* node;
		//release_count = 0;
		//
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

		node = tiles->first;
		for(; sn; --sn) {
			Tile* c = (Tile*)node->data;
			tile_parent(c, &p);
			while(p.z > 0) {
				int has = 0;
				c = tile_find(tiles, &p);
				if(c == 0) {
					for(j = 0; j < t_count; ++j) {
						Tile* tt = &t[j];
						if(tt->z == p.z&&tt->x == p.x&&tt->y == p.y) { has = 1; break; }
					}
					if(!has) t[t_count++] = p;
				} else tile_tofirst_s(tiles_load, c);
				tile_parent(&p, &p);
			}
			node = node->next;
		}
		qsort(t,t_count,sizeof(Tile),cmp_tile);

		/*if(last_t_count != t_count) {
			print("last_t_count: %d\n", last_t_count);
			last_t_count = t_count;
		}*/

		for(j = t_count-1; j >= 0; --j) {
			Tile* newtile = (Tile*)malloc(sizeof(Tile));
			tile_init(newtile, t[j].x, t[j].y, t[j].z);

			newtile->ref += 2;
			deque_push_front(tiles, newtile);
			deque_push_front_s(tiles_load, newtile);

			tiles_limit();

			tile_release(newtile);
		}

		/*if(last_r_count != release_count) {
			print("last_r_count: %d\n", release_count);
			last_r_count = release_count;
		}*/
	}
}

void do_exit(void){
	//destroy_synclist(tiles_get);
//	clear_list(tiles);
	//destroy_list(tiles);
	//destroyMap(&map);
}

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
	glTexImage2D(GL_TEXTURE_2D, 0, 3/*GL_COMPRESSED_RGB*/, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, t->texdata);
	//glTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, t->texdata);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	free(t->texdata);
	t->texdata = 0;
	t->tex = textureId;
	vtx[2] = 0; vtx[3] = 0;
	vtx[6] = 0; vtx[7] = 1;
	vtx[10]= 1; vtx[11]= 0;
	vtx[14]= 1; vtx[15]= 1;
	return 1;
}
#if _WIN32
static DWORD WINAPI worker_load(void* param){
#elif __linux
static void* worker_load(void* param){
#endif
	mtx_t mtx;
	void* data;
	int n = (int)param;
	mtx_init(&mtx);
	while(1){
		//print("%d wait\n",n);
		Tile* t = queue_pop_wait(tiles_load);
		/*Tile* t;
		cnd_wait(&tiles_load->cnd,&tiles_load->mtx);
		t = queue_pop(tiles_load);*/
		//print("%d get\n",n);
		mtx_lock(&mtx);
		if (!tile_release(t)){
			t->ref += 1;
			//if(tiles_loaded->count >= 40) sleep(10);
			mtx_unlock(&mtx);
			//print("%d load\n",n);
			data = getImageData(t);

			mtx_lock(&mtx);
			if (tile_release(t)){
				mtx_unlock(&mtx);
				//print("release after load\n");
				free(data);
			} else {
				//mtx_lock(&mtx);
				t->ref += 1;
				t->texdata = data;
				//queue_push(tiles_loaded, t);
				array_push(tiles_loaded, t);
				mtx_unlock(&mtx);
				//print("%d push\n",n);
			}
		} else {
			//print("thread release\n");
			mtx_unlock(&mtx);
		}
	}
	mtx_destroy(&mtx);
	return 0;
}
int change=0;
void Render(float f){
	int i=0,j=0;
	GLuint ltex=-1;

	Tile* t = array_pop(tiles_loaded);
	while(t) {
		if(t == tiles_loaded->end) {
			print("tiles_loaded end\n");
			t = array_pop(tiles_loaded);
			continue;
		}
		//print("release loaded %p %2d %2d %2d\n", t, t->z, t->x, t->y);
		if(!tile_release(t)) { // clear unused tiles
			tile_make_tex(t);
			if(++i == 2) break; // load by 1 texture or 2 or 5..
		} /*else {
			print("tiles_loaded release\n");
		}*/
		t = array_pop(tiles_loaded);
		change = 1;
	}

	if(change) {
		make_tiles();
		updateQuads();
		change = 0;
		print("tiles_loaded count: %d\n",tiles_loaded->count);
	}

	glClear(GL_COLOR_BUFFER_BIT);

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
	
	i = 0;
	t = array_pop(tiles_release);
	while(t) {
		//print("release        %p %2d %2d %2d\n", t, t->z, t->x, t->y);
		if(t->texdata) {
			free(t->texdata);
			t->texdata = 0;
		}
		if(t->tex) {
			glDeleteTextures(1, &t->tex);
			t->tex = 0;
		}
		free(t);
		if(++i == 4) break;// release by 4 tiles
		t = array_pop(tiles_release);
	}
}

void Draw_empty(void){}

/*double p[][2]={
	{-165.0,60.0},
	{-165.0+5.0,60.0},
	{-165.0+5.0,60.0-5.0},
	{-165.0,60.0-5.0}
};*/

//55.3992609,-163.1431794

const double pi180 = M_PI / 180.0;
double getx(double lon, double zoom) {
	return ((lon + 180) / 360) * pow(2, zoom);
}
double gety(double lat, double zoom) {
	return (1 - log(tan(pi180*lat) + 1 / cos(pi180*lat)) / M_PI) / 2 * pow(2, zoom);
}

double getxp(double lon, double pzoom) {
	return ((lon + 180.0) / 360.0) * pzoom;
}
double getyp(double lat, double pzoom) {
	return (1 - log(tan(pi180*lat) + 1 / cos(pi180*lat)) / M_PI) / 2 * pzoom;
}

#if _MSC_VER < 1900
double log2(double Value) {
	return log(Value) * (1.4426950408889634073599246810019);
}
#endif

//double lon = -163.1441188, lat = 55.3988969;
//double lon = -71.06643059, lat = 42.35431920;
#define NUM_POINTS 4
double points[NUM_POINTS][3] = {
	{-163.1441188, 55.3988969, 18.4},
	{-71.06643059, 42.35431920, 18.4},
	{139.62034836, -17.53323892, 18.4},
	{18.41713920, -32.82673124, 18.4}
};

crd_t fromPointToLatLng(crd_t point, double zoom);
crd_t fromLatLngToPoint(double lat, double lon, double zoom);
void keyboard(unsigned char key,int x,int y){
	if(key == 32) {
//		double z,rx,ry;
		crd_t ret;
		print("row: %.16f column: %.16f zoom: %.16f\n",center.row,center.column,center.zoom);
		//ret = fromLatLngToPoint(lat,lon, center.zoom);
		ret = fromPointToLatLng(center, center.zoom);
		print("lon: %.8f lat: %.8f\n",ret.x,ret.y);
	}
}

/*
public final class GoogleMapsProjection2 
{
private final int TILE_SIZE = 256;
private PointF _pixelOrigin;
private double _pixelsPerLonDegree;
private double _pixelsPerLonRadian;

public GoogleMapsProjection2()
{
	this._pixelOrigin = new PointF(TILE_SIZE / 2.0,TILE_SIZE / 2.0);
	this._pixelsPerLonDegree = TILE_SIZE / 360.0;
	this._pixelsPerLonRadian = TILE_SIZE / (2 * Math.PI);
}

double bound(double val, double valMin, double valMax)
{
	double res;
	res = Math.max(val, valMin);
	res = Math.min(res, valMax);
	return res;
}

double degreesToRadians(double deg)
{
	return deg * (Math.PI / 180);
}

double radiansToDegrees(double rad)
{
	return rad / (Math.PI / 180);
}

PointF fromLatLngToPoint(double lat, double lng, int zoom)
{
	PointF point = new PointF(0, 0);

	point.x = _pixelOrigin.x + lng * _pixelsPerLonDegree;

	// Truncating to 0.9999 effectively limits latitude to 89.189. This is
	// about a third of a tile past the edge of the world tile.
	double siny = bound(Math.sin(degreesToRadians(lat)), -0.9999,0.9999);
	point.y = _pixelOrigin.y + 0.5 * Math.log((1 + siny) / (1 - siny)) *- _pixelsPerLonRadian;

	int numTiles = 1 << zoom;
	point.x = point.x * numTiles;
	point.y = point.y * numTiles;
	return point;
}

PointF fromPointToLatLng(PointF point, int zoom)
{
	int numTiles = 1 << zoom;
	point.x = point.x / numTiles;
	point.y = point.y / numTiles;

	double lng = (point.x - _pixelOrigin.x) / _pixelsPerLonDegree;
	double latRadians = (point.y - _pixelOrigin.y) / - _pixelsPerLonRadian;
	double lat = radiansToDegrees(2 * Math.atan(Math.exp(latRadians)) - Math.PI / 2);
	return new PointF(lat, lng);
}

public static void main(String []args) 
{
	GoogleMapsProjection2 gmap2 = new GoogleMapsProjection2();

	PointF point1 = gmap2.fromLatLngToPoint(41.850033, -87.6500523, 15);
	System.out.println(point1.x+"   "+point1.y);
	PointF point2 = gmap2.fromPointToLatLng(point1,15);
	System.out.println(point2.x+"   "+point2.y);
}
}
*/
#define TILE_SIZE 256
double _pixelOrigin[2] = {128.0,128.0};
double _pixelsPerLonDegree = TILE_SIZE / 360.0;
double _pixelsPerLonRadian = TILE_SIZE / (2.0 * M_PI);

double bound(double val, double valMin, double valMax){
	double res;
	res = maxd(val, valMin);
	res = mind(res, valMax);
	return res;
}

double degreesToRadians(double deg){
	return deg * pi180;
}

double radiansToDegrees(double rad){
	return rad / pi180;
}

crd_t fromLatLngToPoint(double lat, double lon, double zoom){
	crd_t point;// = new PointF(0, 0);
	//double siny;
	double numTiles;

	point.x = getx(lon, zoom);// _pixelOrigin[0] + lon * _pixelsPerLonDegree;

	// Truncating to 0.9999 effectively limits latitude to 89.189. This is
	// about a third of a tile past the edge of the world tile.
	//siny = bound(sin(degreesToRadians(lat)), -0.9999,0.9999);
	//point.y = _pixelOrigin[1] + 0.5 * log((1.0 + siny) / (1.0 - siny)) * -_pixelsPerLonRadian;
	point.y = gety(lat, zoom);

	numTiles = pow(2,zoom);// 1 << zoom;
	//point.x = point.x * numTiles;
	//point.y = point.y * numTiles;
	point.zoom = zoom;
	return point;
}

crd_t fromPointToLatLng(crd_t point, double zoom){
	crd_t ret;
	double lon,latRadians,lat;
	//int numTiles = 1 << zoom;
	double numTiles = pow(2, zoom);
	point.x = point.x / numTiles;
	point.y = point.y / numTiles;

	lon = (point.y - 0.5/*_pixelOrigin[0]*/) / (1/360.0)/*_pixelsPerLonDegree*/;
	latRadians = (point.x - 0.5/*_pixelOrigin[1]*/) / -(1/(2.0 * M_PI))/*_pixelsPerLonRadian*/;
	lat = radiansToDegrees(2 * atan(exp(latRadians)) - M_PI / 2);
	ret.x = lon;
	ret.y = lat;
	ret.zoom = zoom;
	return ret;
}

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

double lerpd(double a, double b, double t) {
	return a + t*(b - a);
}


typedef struct {
	char** data;
	int count;
	int cap;
}Array2;

Array2* make_array2(int count) {
	Array2* a = (Array2*)malloc(sizeof(Array2));
	a->data = malloc(count * sizeof(void*));
	a->count = 0;
	a->cap = count;
	return a;
}

void array_push2(Array2* a, void* data) {
	if(a->count++ >= a->cap) {
		a->cap += 20;
		print("realloc: %d\n", a->cap);
		a->data = realloc(a->data, a->cap * sizeof(void*));
	}
	a->data[a->count - 1] = data;
}

void* array_pop2(Array2* a) {
	if(a->count) return a->data[--a->count];
	return 0;
}
int main(int argc, char* argv[]) {
	int i,ip=0;
	double z,startz,a=0,anim=0.004;
	crd_t crd;
	time_t tm;
	//int policy;
    //struct sched_param param;
	//char* ch;
	//Array2* arr = make_array2(2);
	srand((unsigned int)time(&tm));
	/*array_push2(arr, "1");
	array_push2(arr, "2");
	array_push2(arr, "3");
	ch = array_pop2(arr);
	ch = array_pop2(arr);*/
	
	/*pthread_getschedparam(pthread_self(), &policy, &param);
	print("%d %d\n",policy,param.sched_priority);
    param.sched_priority = sched_get_priority_max(2);
	pthread_setschedparam(pthread_self(), 1, &param);
	print("%d %d\n",policy,param.sched_priority);
	param.sched_priority = sched_get_priority_min(2);
	print("%d %d\n",policy,param.sched_priority);*/

	glutInitWindowSize(veiwport[0], veiwport[1]);
	glutInit(&argc, argv);
	glutInitDisplayMode(GLUT_RGB | GLUT_DOUBLE);
	glutCreateWindow("glutplanet");
	glutMouseFunc(mouse);
	glutMotionFunc(mousemove);
	glutKeyboardFunc(keyboard);
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
	startz = center.zoom;
	crd = fromPointToLatLng(center, startz);

	tiles = make_queue();
	tiles_load = make_queue();
	tiles_loaded = make_array(64);
	tiles_release = make_array(64);
	//mtx_init(&g_mtx);

	i = 3;// num_cores();
	while(i--) StartThread(worker_load,i);
	
	make_tiles();
#endif

	z = startz;
	for (;;){
		glutMainLoopEvent();

		//if (z<points[ip][3]) {
		/*{
			double zz,rx,ry;
			a +=anim;
			if(a > 1) { a = 1; anim = -anim; }
			if(a < 0) { 
				a = 0; anim = -anim; 

				if(ip++ >= NUM_POINTS-1) ip = 0;
			}
			center.zoom = lerpd(startz, points[ip][2], a);
			zz = pow(2,center.zoom);
			rx = getxp(points[ip][0],zz);
			ry = getyp(points[ip][1],zz);
			//rx = lerpd(rx, getxp(0, zz),1-a);
			//ry = lerpd(ry, getyp(0, zz),1-a);
			crd_set(&center,ry,rx);

			make_tiles();
			updateQuads();
		}*/

		Render(0);
		glutSwapBuffers();
	}

	return 0;
}
