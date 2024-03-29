#include <stdio.h>
#include <iostream>
#include <stdlib.h>
#include <fstream>
#include <unistd.h>
#include <bits/stdc++.h>
#include <string>

#define MAX_FRAMES 128
#define MAX_PT 64

using namespace std;

struct VMA{
        int start_vp;
        int end_vp;
        unsigned int write_protected;
        unsigned int file_mapped;
}vma;

struct PTE{
	unsigned int valid:1;
	unsigned int referenced:1;
	unsigned int modified:1;
	unsigned int writeprotected:1;
	unsigned int pagedout:1;
	unsigned int frame:7; //max_num of frames is 128(2^7)i
	//additional bits
	unsigned int isinvma:1;
	unsigned int vmachecked:1;
	unsigned int filemapped:1;
	unsigned int nruclass:2; //0,1,2,3
}pte;

struct P_STATS{
	unsigned long segv;
        unsigned long segprot;
        unsigned long maps;
        unsigned long unmaps;
        unsigned long ins;
        unsigned long outs;
        unsigned long fins;
        unsigned long fouts;
        unsigned long zeros;
}pstats;

class Proc{
public:

	int pid;
        vector<VMA> vmas;
        vector<PTE> pagetable; //represents the translations from virtual pages to physical frames for that process
	P_STATS stats;

	Proc(int id){
		pid = id;
		for(int i = 0; i < MAX_PT; i++){
			PTE pte = {0};
			pagetable.push_back(pte);
		}

		stats = {0};	
	}


	void update_presence(int vaddr, PTE* pte){ //update at the first page fault
		pte->vmachecked = 1;
		for(int i = 0; i < vmas.size(); i++){
			if(vaddr <= vmas[i].end_vp && vaddr >= vmas[i].start_vp){
				pte->isinvma = 1;
                                pte->filemapped = vmas[i].file_mapped;
                                pte->writeprotected = vmas[i].write_protected;
				pte->valid = 1;
				return;
			}
		}		
	}


};

struct FRAME{
	unsigned int frameidx;
	unsigned int age;
	unsigned int lastused;
        Proc* p;
        int vpage;
}frame;

FILE* fp_input;
FILE* fp_rfile;
int ofs;
vector<int> randvals;

//print options
int oflag;
int pflag;
int fflag;
int sflag;
bool x, y, f = false;

vector<Proc*> processes;
FRAME* frametable[MAX_FRAMES] = {};
deque<FRAME*> freelist;

int num_frames;
unsigned long long cost = 0;
unsigned long ctx_switches = 0;
unsigned long proc_exits = 0;
unsigned long inst_num = 0;

FRAME* get_frame();
bool get_next_instruction(char& operation, int& vpage);
int myrandom(int size);
void print_pagetable(Proc* p);
void print_frametable();
void print_stats(Proc* p);
void print_summary();


class Pager {
public:
	virtual FRAME* select_victim_frame() = 0; // virtual base class
};

Pager* pager;

class FIFO: public Pager{
public:
	int hand;

	FIFO(){
		hand = 0;
	}

	FRAME* select_victim_frame(){
		FRAME* fm = frametable[hand];
		hand++;
		if(hand == num_frames) hand = 0;
		return fm;
	}	

};

class Random: public Pager{
public:
	FRAME* select_victim_frame(){

		int idx = myrandom(num_frames);
		FRAME* fm = frametable[idx];
		return fm;
	}
};

class Clock: public Pager{
public:
	int hand;

	Clock(){
		hand = 0;
	}

	FRAME* select_victim_frame(){
		FRAME* fm = nullptr;
		bool found = false;
		while(!found){
			fm = frametable[hand];
			if(fm->p->pagetable[fm->vpage].referenced == 0){ //evict
				hand++;
				if(hand == num_frames) hand = 0;
				found = true;
			}
			else{
				fm->p->pagetable[fm->vpage].referenced = 0;
				hand++;	
				if(hand == num_frames) hand = 0;	
			}
		}
		return fm;
	}
};

class NRU: public Pager{
public:
	int hand;
	int clock;

	NRU(){
		hand = 0;
		clock = inst_num;
	}

	FRAME* select_victim_frame(){

		FRAME* fm = nullptr;
		PTE* pte = nullptr;
		unsigned int min_class_num = UINT_MAX;

		
		for(int i = 0; i < num_frames; i++){
			pte = &frametable[i]->p->pagetable[frametable[i]->vpage];
			if(pte->referenced == 0 && pte->modified == 0){
				pte->nruclass = 0;
			}
			else if(pte->referenced == 0 && pte->modified == 1){
				pte->nruclass = 1;
			}
			else if(pte->referenced == 1 && pte->modified == 0){
				pte->nruclass = 2;
			}
			else{
				pte->nruclass = 3;
			}
			if(min_class_num > pte->nruclass) min_class_num = pte->nruclass;
		}

		bool found = false;
		while(!found){
			fm = frametable[hand];
			unsigned int nruclass = fm->p->pagetable[fm->vpage].nruclass;
			if(nruclass == min_class_num){
				found = true;
			}
			hand++;
			if(hand == num_frames) hand = 0;
		}
		
		if(inst_num - clock + 1 >= 50){
			//reset refbit
			for(int i = 0; i < num_frames; i++){
				if(frametable[i]->p->pagetable[frametable[i]->vpage].valid)	
					frametable[i]->p->pagetable[frametable[i]->vpage].referenced = 0;
			}
			clock = inst_num + 1;
		}		
		return fm;
	}
};

class Aging: public Pager{
public:
	int hand;

	Aging(){
		hand = 0;
	}

	FRAME* select_victim_frame(){
		FRAME* fm = nullptr;
		unsigned int minage = UINT_MAX;
		for(int i = 0; i < num_frames; i++){
			frametable[i]->age = frametable[i]->age >> 1; 

			// add R bit as the leftmost bit
			unsigned int r = frametable[i]->p->pagetable[frametable[i]->vpage].referenced;
			if(r == 1){  // shift counter right by 1
				frametable[i]->age = frametable[i]->age | 0x80000000;	
			}

			if(frametable[i]->age <= minage){
				minage = frametable[i]->age;
			}
			//reset R bit
			frametable[i]->p->pagetable[frametable[i]->vpage].referenced = 0;	
		}
				
		bool found = false;
		while(!found){
			if(frametable[hand]->age == minage){
				fm = frametable[hand];
				found = true;
			}		
			hand++;
			if(hand == num_frames) hand = 0;
		}
		return fm;
	}
};

class Workingset: public Pager{
public:
	int hand;
	unsigned long long clock;
	Workingset(){
		hand = 0;
		clock = 0;
	}

	FRAME* select_victim_frame(){
		FRAME* fm = nullptr;
		unsigned int oldest = UINT_MAX;
		int oldest_idx = -1;
		int idx = hand;
		int cnt = 0;
		bool found = false;
		while(cnt < num_frames){
			//cout << frametable[hand]->frameidx << "(" << frametable[hand]->p->pagetable[frametable[hand]->vpage].referenced << " " << frametable[hand]->p->pid << ":" << frametable[hand]->vpage << " " << frametable[hand]->lastused << ") ";

			unsigned int r = frametable[hand]->p->pagetable[frametable[hand]->vpage].referenced;
/**
			if(oldest > frametable[hand]->lastused){
                        	oldest_idx = hand;
                                oldest = frametable[hand]->lastused;
                        }
**/
			if(r == 0){
				if(inst_num - frametable[hand]->lastused >= 50){
					fm = frametable[hand];
					found = true;
					break;
				}
				else{
					if(oldest > frametable[hand]->lastused){
		                                oldest_idx = hand;
                	                oldest = frametable[hand]->lastused;
                		        }
				}
			}
			else{ //r == 1, record curtime and reset R
				frametable[hand]->lastused = inst_num;
				frametable[hand]->p->pagetable[frametable[hand]->vpage].referenced = 0;			
			}

			cnt++;
			hand++;
			if(hand == num_frames) hand = 0;
		}

		if(!found){
			if(oldest_idx == -1){
				cnt = 0;
				while(cnt < num_frames){
					if(oldest > frametable[hand]->lastused){
                                                oldest_idx = hand;
                                                oldest = frametable[hand]->lastused;
                                        }	
					cnt++;
					hand++;
                        		if(hand == num_frames) hand = 0;
				}
			}
               		fm = frametable[oldest_idx];
                       	hand = oldest_idx;
                       	hand++;
                       	if(hand == num_frames) hand = 0;
               	}
		hand = fm->frameidx + 1;
		if(hand == num_frames) hand = 0;	
		//cout << "| " << fm->frameidx << " " <<  oldest_idx <<  endl;
		return fm;
	}
};


//describes the usage of each of its physical frames
//reverse mappings to the process and the vpage that maps a particular frame

FRAME* get_frame(){
        FRAME* frame = nullptr;
        if(freelist.empty()){
		frame = pager->select_victim_frame();
	}else{
		frame = freelist.front();
		freelist.pop_front();
	}
	return frame;
}

bool get_next_instruction(char& operation, int& vpage){
	char* buff = NULL;
        size_t len = 0;
	while(getline(&buff, &len, fp_input) != -1){
		if(buff[0] == '#') continue;
	
		sscanf(buff, "%c %d", &operation, &vpage);
		return true;
	}	
	return false;
}


int myrandom(int size){
        int randnum = randvals[ofs] % size;
        if(ofs + 1 == randvals.size()) ofs = 0;
        else ofs += 1;

        return randnum;
}

void do_map(Proc* target_p, PTE* pte, int vpage, FRAME* newframe){

	if(pte->pagedout){
		target_p->stats.ins += 1;
		if(oflag) cout << " IN" << endl;
		cost += 3100; 
	}
	if(pte->filemapped){
		target_p->stats.fins += 1;
		if(oflag) cout << " FIN" << endl;
		cost += 2800;
	}
	if(pte->pagedout == 0 && pte->filemapped == 0){
                //ZERO
                target_p->stats.zeros += 1;
                if(oflag) cout << " ZERO" << endl;
		cost += 140;
        }
	//allocate a frame
	newframe->vpage = vpage;
        newframe->p = target_p;

        pte->frame = newframe->frameidx;
	pte->valid = 1;
	target_p->stats.maps += 1;
	newframe->age = 0;
	newframe->lastused = inst_num;
}


void unmap(Proc* target_p, PTE* pte, FRAME* newframe, bool exit){

	if(pte->modified && !pte->filemapped && !exit){ //need to access disk
		pte->pagedout = 1;	
		target_p->stats.outs += 1; //OUT
		if(oflag) cout << " OUT" << endl;
		cost += 2700;
        }
        if(pte->modified && pte->filemapped){
		//FOUT	
		target_p->stats.fouts += 1;
		if(oflag) cout << " FOUT" << endl;
		cost += 2400;
	}        

       	newframe->p = nullptr;
        newframe->vpage = NULL;

	pte->valid = 0;
	pte->frame = 0;
	//reset pte
	pte->modified = 0;
	pte->referenced = 0;
    	
	target_p->stats.unmaps += 1;

	if(exit){
		freelist.push_back(newframe);
	}
}

void simulate(Proc* cur){
	char operation;
	int vpage;
        while(get_next_instruction(operation, vpage)){
		if(oflag) cout << inst_num << ": ==> " << operation << " " << vpage << endl;
		if(operation == 'c'){

			for(int i = 0; i < processes.size(); i++){
				if(processes[i]->pid == vpage){
					cur = processes[i];
				}
			}
			ctx_switches++;
			cost += 130;
		}
		else if(operation == 'e'){
			//TODO On process exit, traverse the active process’s page table 
			//and for each valid entry UNMAP the page and FOUT modified filemapped pages. 
			cout << "EXIT current process " << cur->pid << endl;
			for(int i = 0; i < MAX_PT; i++){
				PTE *pte = &cur->pagetable[i];
				
				if(pte->valid){
					if(oflag) cout << " UNMAP " << cur->pid << ":" << i << endl;
                                	cost += 400;
					unmap(cur, pte, frametable[pte->frame], true);
				}
				cur->pagetable[i] = {0};
			}
			//Note that dirty non-fmapped (anonymous) pages are not written back (OUT) as the process exits. The used frame has to be returned to the free pool 
			proc_exits++;
			cost += 1250;
			inst_num++;
			return;		
		}
		else{ //read/write
			cost += 1;
                	PTE *pte = &cur->pagetable[vpage];
                	if(!pte->valid){
                 	// this in reality generates the page fault exception and now you execute
				if(!pte->vmachecked) cur->update_presence(vpage, pte);
				
				if(!(pte->isinvma) && pte->vmachecked){
					//SEGV out
                                	cur->stats.segv += 1;
					cost += 340;
					if(oflag) cout << " SEGV" << endl;
					inst_num++;
                                	continue;
				}

                        	FRAME *newframe = get_frame();
				//cout << newframe->frameidx << " " << newframe->vpage << endl;
				//if it was mapped, unmap(victim) and then map
				if(newframe->p){
					Proc* target_p = processes[newframe->p->pid]; 
					PTE* oldpte = &target_p->pagetable[newframe->vpage];
					if(oflag) cout << " UNMAP " << target_p->pid << ":" << newframe->vpage << endl;
					cost += 400;
					unmap(target_p, oldpte, newframe, false);		
				}
				
				do_map(cur, pte, vpage, newframe);
				if(oflag) cout << " MAP " << newframe->frameidx << endl;
				cost += 300;
        		}

			//Now set REFERENCED and MODIFIED bits
                	// check write protection
			if(operation == 'w'){
				//SEGPROT
				if(pte->writeprotected){
					cur->stats.segprot += 1;
					if(oflag) cout << " SEGPROT" << endl;
					cost += 420;
					pte->referenced = 1;
					pte->modified = 0;
				}
				else{
					pte->referenced = 1;
					pte->modified = 1;
				}
			}
			else if(operation == 'r'){
				pte->referenced = 1;	
			}

		}
	inst_num++;
	if(f) print_frametable();
	if(x) print_pagetable(cur);
	if(y){
		for(int i = 0; i < processes.size(); i++) print_pagetable(processes[i]);
	} 
	}

}


void print_pagetable(Proc* p){
	cout << "PT[" << p->pid << "]: "; 
	for(int i = 0; i < MAX_PT; i++){
		string bits = "";
		PTE pte = p->pagetable[i];
		if(pte.valid == 0){//PTEs that are not valid ‘#’ if have been swapped out 
			if(pte.pagedout == 0) cout << "* ";
			else cout << "# ";
			continue;
		}
		cout << i << ":";
		if(pte.referenced) bits += "R";
		else bits += "-";

		if(pte.modified) bits += "M";
		else bits += "-";

		if(pte.pagedout) bits += "S";
		else bits += "-";
		
		cout << bits << " ";
	}
	cout << endl;
}


void print_frametable(){
	cout << "FT: ";
	for(int i = 0; i < num_frames; i++){
		if(frametable[i]->p != nullptr) cout << frametable[i]->p->pid << ":" << frametable[i]->vpage << " ";
		else cout << "*" << " ";
	}
	cout << endl;
}

void print_stats(Proc* p){
	printf("PROC[%d]: U=%lu M=%lu I=%lu O=%lu FI=%lu FO=%lu Z=%lu SV=%lu SP=%lu\n", p->pid, p->stats.unmaps, p->stats.maps, p->stats.ins, p->stats.outs, p->stats.fins, p->stats.fouts, p->stats.zeros, p->stats.segv, p->stats.segprot);
}

void print_summary(){
	printf("TOTALCOST %lu %lu %lu %llu %lu\n", inst_num, ctx_switches, proc_exits, cost, sizeof(PTE));

}


int main(int argc, char *argv[]){
	ofs = 0;
	oflag = 0;
	pflag = 0;
	fflag = 0;
	sflag = 0;

	char* algo = NULL;
	char* options = NULL;
	int tok;
	while((tok = getopt(argc, argv, "f:a:o:")) != -1){
		switch(tok){
			case 'a':
				algo = optarg;
				switch(optarg[0]){
					case 'f':
						pager = new FIFO();
						break;
					case 'r':
						pager = new Random();
						break;
					case 'c':
						pager = new Clock();
						break;
					case 'e':
						pager = new NRU();
						break;
					case 'a':
						pager = new Aging();
						break;
					case 'w':
						pager = new Workingset();
						break;
				}
				break;
			case 'f':
                                num_frames = atoi(optarg);
                                break;
			case 'o':
				options = optarg;
				for(int i = 0; i < strlen(options); i++){
					switch(optarg[i]){
						case 'O':
							oflag = 1;
							break;
						case 'P':
							pflag = 1;
							break;
						case 'F':
							fflag = 1;
							break;
						case 'S':
							sflag = 1;
							break;
						case 'x':
							x = true;
							break;
						case 'y':
							y = true;
							break;
						case 'f':
							f = true;
							break;
						case 'a':
							break;
						default:
							break;
					}
				}
				break;
			default:
				abort();
		}
	}

	//initialize frametable 
	for(int i =0; i < num_frames; i++){
		FRAME* f = (FRAME*)malloc(sizeof(FRAME));
		f->frameidx = i;
		f->p = nullptr;
		f->age = 0;
		f->lastused = 0;
		freelist.push_back(f);
		frametable[i] = f;
	}
	int argidx = optind;

	if(argc > argidx){
		fp_input = fopen(argv[argidx], "r");
		argidx++;
	}	
	
	if(argc > argidx){
		fp_rfile = fopen(argv[argidx], "r");
		argidx++;
	}

	char* buff = NULL;
	size_t len = 0;

	getline(&buff, &len, fp_rfile);
	while(getline(&buff, &len, fp_rfile) != -1){
		randvals.push_back(atoi(buff));
	}	

	buff = NULL;
	len = 0;
	int num_proc = -1;
	while(getline(&buff, &len, fp_input) != -1){
		if(buff[0] == '#') continue;
		if(num_proc < 0){
			num_proc = atoi(buff);

			int i = 0;
			while(i < num_proc){
				getline(&buff, &len, fp_input);
				if(buff[0] == '#') continue;

				Proc* p = new Proc(i);
				processes.push_back(p);
				int num_vmas = atoi(buff);
				
				int j = 0;
				while(j < num_vmas){
					getline(&buff, &len, fp_input);
					if(buff[0] == '#') continue;	

                                        vector<unsigned int> tmp;
					char* tok = strtok(buff, " ");
					while(tok != NULL){
						tmp.push_back(atoi(tok));
						tok = strtok(NULL, " ");		
					}
					VMA vma_el = {tmp[0], tmp[1], tmp[2], tmp[3]};
					p->vmas.push_back(vma_el);
					j++;	
				}
				i++;
			}

			break;
		}
	}

	for(int i = 0; i < processes.size(); i++){
		simulate(processes[i]);
	}

	if(pflag){
		for(int i = 0; i < processes.size(); i++){
			print_pagetable(processes[i]);
		}
	}	
	if(fflag) print_frametable();
	if(sflag){
		for(int i = 0; i < processes.size(); i++){
			print_stats(processes[i]);	
		}
	}
	if(sflag) print_summary();	

}
