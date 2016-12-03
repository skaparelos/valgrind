
/*--------------------------------------------------------------------*/
/*--- TLB Measuring & Page Tracking Tool                  cg_tlb.c ---*/
/*--------------------------------------------------------------------*/

/*
 This file is part of Cachegrind, a Valgrind tool for cache
 profiling programs.
 
 Copyright (C) 2002-2012 Nicholas Nethercote
 njn@valgrind.org
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License as
 published by the Free Software Foundation; either version 2 of the
 License, or (at your option) any later version.
 
 This program is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 02111-1307, USA.
 
 The GNU General Public License is contained in the file COPYING.
 */


#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_debuginfo.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_options.h"
#include "pub_tool_machine.h"     // VG_(fnptr_to_fnentry)
#include "pub_tool_mallocfree.h"


#define TLB_TYPE_ITLB  (0)
#define TLB_TYPE_DTLB  (1)
#define TLB_TYPE_L2TLB (2)


/*------------------------------------------------------------*/
/*--- Paging Information                                   ---*/
/*------------------------------------------------------------*/

/* This structure is used to hold all pages accessed and the times of access.
 * This is done only to show to the user.
 * To be used only if option --show-pages is enabled.
 * It is set dynamically (linked list), since the number of pages cannot be known from before */
typedef struct pages_accessed{
    Addr tag;
    ULong count; //ULong cause it might get big enough
    struct pages_accessed *next;
}PAGES;


/*------------------------------------------------------------*/
/*--- TLB Simulation                                       ---*/
/*------------------------------------------------------------*/

/* Holds TLB characteristics */
typedef struct _tlb_t{
    
    //Holds hits and misses per TLB
    Int hit,miss;
    
    //Page size should always be in B.
    //i.e. if page size is 4KB then PAGE_SIZE=4*1024=4096
    ULong page_size;
    
    /* assoc represents associativity.
     * -1 is Fully Associative
     *  0 is Direct Mapped
     *  N where N>0 is N-way Associative */
    Int assoc;
    Int entries;
    
    ULong    offset_mask;    //Used to separate Virtual Address Offset
    ULong    vpn_mask;       //Used to separate Virtual Address VPN
    ULong    index_mask;     //used in direct map, to split VPN to tag and index.
    ULong    tag_mask;       //Used in direct map, to get the tag from VPN.
    Int      sets;           //Used in N-way assoc, to hold the number of sets
    
    Bool     replace;        //True if TLB was full and from now on a replacing algorithm has to choose what to replace. Used in FA.
    
    PAGES    *page_ptr;
    Int      total_pages;
    
    Int      tlb_counter;    //Points to which entry is currently the TLB operating
    
    Char    desc_line[128];  //Holds the tlb configuration in words.
}tlb_t;


/*
 *TLBc[0] -> iTLB
 *TLBc[1] -> dTLB
 *TLBc[2] -> L2TLB
 */
static tlb_t TLBc[3];


/* This structure represents a TLB entry.
 * Holds the tag and the times it was accessed.
 * An array of TLB_ENTRYs consists a TLB */
typedef struct _tlb_entry{
    Addr tag;
    Int count;
}TLB_ENTRY;

//TLB Array
TLB_ENTRY **TLB;


/*------------------------------------------------------------*/
/*--- Command Line Arguments                               ---*/
/*------------------------------------------------------------*/
static Bool clo_sim_tlb=True;       /* Simulate TLB     */
static Bool clo_sim_pages=False;    /* Simulate Pages   */

/*------------------------------------------------------------*/
/*--- Simulation General Info                              ---*/
/*------------------------------------------------------------*/

/* Virtual Address Space Size */
static Int VAS_SIZE=32; //initially set to 32 since it's the most common.

/* Replacement Policy */
/*
 * 0 -> LFU
 * 1 -> LRU
 * 2 -> RR
 */
static Int RepPol=1; //LRU is the default

/* Which TLBs to simulate? (iTLB, dTLB or L2TLB) */
static Bool sim_iTLB=False;
static Bool sim_dTLB=False;
static Bool sim_L2TLB=False;

/* TLB_TYPE distinguishes between iTLB, dTLB, L2TLB (unified) or all.
 * -1 -> all   //Not used anymore
 *  0 -> iTLB
 *  1 -> dTLB
 *  2 -> L2TLB */
static Int TLB_TYPE=-1; //this will change soon enough to point to the valid TLB. No worries it's -1.


/*------------------------------------------------------------*/
/*--- Function Definitions                                 ---*/
/*------------------------------------------------------------*/

Int log2(Int);
void add_page(Addr);
void print_pages(void);

void do_miss(ULong*, ULong*);
void do_hit(void);

void TLB_simulation(Addr, ULong*, ULong*);
void tlb_lookup(Addr, Addr, Addr, ULong*, ULong*);

void reference_address(Addr, Int, ULong*, ULong*);

Int LFU(Int);
Int LRU(Int);
void increase_LRU(Int);
Int get_random(Int);

void print_tlb(Int);
//void print_tlb_contents(void);
void tlb_chars(void);
ULong calc_VPN_MASK(Int, ULong);
void tlb_post_clo_init(void);
void print_stats(Int, Int);
void tlbsim_init(Int, Int, Int, Int);
Bool isTLBsim(void);

/*----------------------------------------------------------------------------------------------*/

Bool isTLBsim(void){
    return clo_sim_tlb;
}

/*----------------------------------------------------------------------------------------------*/

/* Adds a page in PAGES linked list */
void add_page(Addr tag){
    
    PAGES *cur=TLBc[TLB_TYPE].page_ptr;
    
    //search if Page already exists
    while(cur!=NULL){
        if(cur->tag==tag){
            cur->count++;
            return;
        }
        cur=cur->next;
    }
    
    // Create new page.
    cur=(PAGES*)VG_(malloc)("pages.1",sizeof(PAGES));
    cur->next=TLBc[TLB_TYPE].page_ptr;
    cur->tag=tag;
    cur->count=1;
    TLBc[TLB_TYPE].page_ptr=cur;
    TLBc[TLB_TYPE].total_pages++;
    return;
}

/*----------------------------------------------------------------------------------------------*/

/* Called by cg_fini() to print page information on screen */
/* And to free all page entries */
void print_pages(void){
    VG_(umsg)("---Pages Accessed---\n");
    Int i;
    for(i=0;i<3;i++){
        if( (sim_iTLB && i==0) || (sim_dTLB && i==1) || (sim_L2TLB && i==2)){
            
            switch(i){
                case 0: VG_(umsg)("\niTLB Pages Accesed\n"); break;
                case 1: VG_(umsg)("\ndTLB Pages Accesed\n"); break;
                case 2: VG_(umsg)("\nL2TLB Pages Accesed\n"); break;
            }
            //Print total pages accessed
            VG_(umsg)("Pages Accessed In total:   %d\n",TLBc[i].total_pages);
            
            //print each page and times it was accessed
            Int i2=0;
            PAGES *cur=TLBc[i].page_ptr;
            while(cur!=NULL){
                VG_(umsg)("%d) Page %08lx, accessed %llu times\n",i2+1,cur->tag,cur->count);
                i2++;
                cur=cur->next;
            }
            
            //free all malloced pages
            while(TLBc[i].page_ptr!=NULL){
                cur=TLBc[i].page_ptr->next;
                VG_(free)(TLBc[i].page_ptr);
                TLBc[i].page_ptr=cur;
            }
            //VG_(umsg)("Freed everything\n");
        }
    }
    
}

/*----------------------------------------------------------------------------------------------*/

//This function is used for debugging.
/*void print_tlb_contents(void){
    
    Int i;
    for(i=0;i<TLBc[TLB_TYPE].entries;i++)
        VG_(umsg)("%d.|_____%d_____|\n",i,TLB[TLB_TYPE][i].tag);
}*/
/*----------------------------------------------------------------------------------------------*/

//set target count equal to 0
//increase everything else by 1
void increase_LRU(Int target){
    
    
    Int i;
    
    //Fully associative
    /* For FA increase all entries by 1 and set most recently used to 0 */
    if(TLBc[TLB_TYPE].assoc==-1){
        
        //Increase count in all elements by one
        for(i=0;i<TLBc[TLB_TYPE].entries;i++){
            TLB[TLB_TYPE][i].count++;
        }
        
        //set target count to 0, as it is the most recently used
        TLB[TLB_TYPE][target].count=0;
        
    }
    
    //N-way set associative
    /* For Nway increase count in all entries in the set by 1 and set the most recently used count to 0*/
    if(TLBc[TLB_TYPE].assoc>0){
        
        //find the set in which the target belongs
        Int set=(Int)(target/TLBc[TLB_TYPE].assoc);
        
        //increase count in elements in the set
        for(i=set*TLBc[TLB_TYPE].assoc;i<(set+1)*TLBc[TLB_TYPE].assoc;i++)
            TLB[TLB_TYPE][i].count++;
        
        //set target count to 0
        TLB[TLB_TYPE][target].count=0;
    }
    
}

/*----------------------------------------------------------------------------------------------*/

/* LRU - Least Recently Used
 Returns the position of the TLB element that was used the least recently */
Int LRU(Int set){
    
    Int pos=-1,i,max; //max holds the max count i.e. the Least Recently Used
    
    /* For fully associative, scan all and return the biggest */
    if(TLBc[TLB_TYPE].assoc==-1){
        //set max to equal the first
        max=TLB[TLB_TYPE][0].count;
        //making sure pos is equal 0. This is only a pre caution for later changes, since otherwise,
        //it might go unnoticed
        pos=0;
        
        for(i=1;i<TLBc[TLB_TYPE].entries;i++){
            if(TLB[TLB_TYPE][i].count>max){
                max=TLB[TLB_TYPE][i].count;
                pos=i;
            }
        }
        return pos;
    }
    
    
    if(TLBc[TLB_TYPE].assoc>0){
        //set max to equal the count in first item in the set
        max=TLB[TLB_TYPE][set*TLBc[TLB_TYPE].assoc].count;
        
        //set pos to equal the first item
        pos=set*TLBc[TLB_TYPE].assoc;
        
        
        for(i=set*TLBc[TLB_TYPE].assoc+1;i<TLBc[TLB_TYPE].assoc*(set+1);i++){
            if(TLB[TLB_TYPE][i].count > max){
                max=TLB[TLB_TYPE][i].count;
                pos=i;
            }
        }
        return pos;
    }
    return pos;
}


/*----------------------------------------------------------------------------------------------*/

/* LFU - Least Frequently Used
 * Returns the position of the TLB element that is used the less */
Int LFU(Int set){
    
    Int i,pos=-1,hold;
    
    
    // Fully Associative
    /* Scan all entries and return the one with the smallest count */
    if(TLBc[TLB_TYPE].assoc==-1){
        //hold holds the first entry count
        hold=TLB[TLB_TYPE][0].count;
        //making sure pos is equal 0. This is only a pre caution for later changes, since otherwise,
        //it might go unnoticed
        pos=0;
        //scan the rest and find the one with the smallest count.
        for(i=1;i<TLBc[TLB_TYPE].entries;i++){
            if(TLB[TLB_TYPE][i].count < hold){
                hold=TLB[TLB_TYPE][i].count;
                pos=i;
            }
        }
    }
    
    
    //N-way associative
    /* Scan all entries in the set and return the one with the smallest count */
    if(TLBc[TLB_TYPE].assoc>0){
        
        //hold should hold the count of the first item in the set
        hold=TLB[TLB_TYPE][set*TLBc[TLB_TYPE].assoc].count;
        //set pos to equal the first item in the set
        pos=set*TLBc[TLB_TYPE].assoc;
        //Scan for smaller
        for(i=set*TLBc[TLB_TYPE].assoc+1;i<TLBc[TLB_TYPE].assoc*(set+1);i++){
            if(TLB[TLB_TYPE][i].count < hold){
                hold=TLB[TLB_TYPE][i].count;
                pos=i;
            }
        }
    }
    
    return pos;
}

/*----------------------------------------------------------------------------------------------*/

//generates a random number in the range 0-range
Int get_random(Int range){
    return VG_(random)(NULL)%range;
}

/*----------------------------------------------------------------------------------------------*/

void do_hit(void){
    TLBc[TLB_TYPE].hit++;
}

/*----------------------------------------------------------------------------------------------*/

void do_miss(ULong *t1, ULong *t2){
    
    TLBc[TLB_TYPE].miss++;
    if(TLB_TYPE==TLB_TYPE_ITLB||TLB_TYPE==TLB_TYPE_DTLB){
        (*t1)++;
    }
    if(TLB_TYPE==TLB_TYPE_L2TLB){
        (*t2)++;
    }
    
}

/*----------------------------------------------------------------------------------------------*/

//checking for L2TLB?
Bool l2check=False;
//Did L2 result in a hit?
Bool l2hit=False;

//addr_tag should always be tag (in DM) or VPN in FA
//addr index
void tlb_lookup(Addr addr_l2,Addr addr_tag, Addr addr_index, ULong *t1, ULong *t2){
    
    
    //fully associative. entries can go anywhere, the entire TLB is looked up.
    if(TLBc[TLB_TYPE].assoc==-1){
        
        Int i=0;
        
        //Iterate through all entries and if found increase count
        //otherwise, deal with the miss
        for(;i<TLBc[TLB_TYPE].entries;i++){
            if(TLB[TLB_TYPE][i].tag==addr_tag){ //hit
                if(l2check)
                    l2hit=True;
                do_hit();
                if(RepPol==0)//LFU
                    TLB[TLB_TYPE][i].count++;
                if(RepPol==1)//LRU
                    increase_LRU(i);
                return;
            }
        }
        
        //miss
        do_miss(t1,t2);
        
        //if checking for l2
        //set it l2hit=false since
        //if execution reached this line
        //then a miss has occurred
        if(l2check)
            l2hit=False;
        
        if(sim_L2TLB){
            Int save_tlb_type=TLB_TYPE;
            TLB_TYPE=TLB_TYPE_L2TLB;
            //we are now checking for L2 TLB.
            l2check=True;
            //avoid infinite loops
            sim_L2TLB=False;
            
            //tlb_lookup(addr_l2, addr_tag, addr_index, t1, t2);
            //call tlb_simulation again for L2, to possibly use different masks
            //and to save page if page tracking is on
            TLB_simulation(addr_l2,t1,t2);
            
            sim_L2TLB=True;
            //We no more check for L2 TLB
            l2check=False;
            TLB_TYPE=save_tlb_type;
        }

        //Do replacements only if L2 TLB resulted in a miss or it doesn't exist
        if(sim_L2TLB==False || l2hit==False){
            if(RepPol==0){ //LFU
                Int entry=LFU(-1);
                TLB[TLB_TYPE][entry].tag=addr_tag;
                TLB[TLB_TYPE][entry].count=1;
                
            }
            if(RepPol==1){ //LRU
                Int entry=LRU(-1);
                TLB[TLB_TYPE][entry].tag=addr_tag;
                TLB[TLB_TYPE][entry].count=0;
                increase_LRU(entry);
                
            }
            if(RepPol==2){ //Random
                //generate a random number in the range 0-entries and use it to replace
                Int entry=get_random(TLBc[TLB_TYPE].entries);
                TLB[TLB_TYPE][entry].tag=addr_tag;
                TLB[TLB_TYPE][entry].count=1;
            }
        }
        return;
    }
    
    //Direct Mapped. entries can only go (tag mod TLB_ENTRIES)
    if(TLBc[TLB_TYPE].assoc==0){
        
        Int entr=TLBc[TLB_TYPE].entries;
        
        if(TLB[TLB_TYPE][addr_index%entr].tag==addr_tag){
            if(l2check)
                l2hit=True;
            do_hit();
        }else{
            do_miss(t1,t2);
            
            if(l2check)
                l2hit=False;
            
            if(sim_L2TLB){
                Int save_tlb_type=TLB_TYPE;
                TLB_TYPE=TLB_TYPE_L2TLB;
                //we are now checking for L2 TLB.
                l2check=True;
                //avoid infinite loops
                sim_L2TLB=False;
                
                //tlb_lookup(addr_l2, addr_tag, addr_index, t1, t2);
                //call simulation again for L2, to possibly use different masks
                //and to save page if page tracking is on
                TLB_simulation(addr_l2,t1,t2);
                
                sim_L2TLB=True;
                //We no more check for L2 TLB
                l2check=False;
                TLB_TYPE=save_tlb_type;
            }
            
            //Do replacements only if L2 TLB resulted in a miss or it doesn't exist
            if(sim_L2TLB==False || l2hit==False){
                TLB[TLB_TYPE][addr_index%entr].tag=addr_tag;
            }
        }
        return;
    }
    
    
    //Nway ASSOC.
    if(TLBc[TLB_TYPE].assoc>0){
        //Go through all entries in the set
        Int i;
        //if(checkL2)
        //  VG_(umsg)("Range to check:%d-%d\n",addr_index*TLBc[TLB_TYPE].assoc,addr_index*TLBc[TLB_TYPE].assoc+TLBc[TLB_TYPE].assoc);
        
        for(i=0;i<TLBc[TLB_TYPE].assoc;i++){
            
            if(TLB[TLB_TYPE][(addr_index*TLBc[TLB_TYPE].assoc)+i].tag==addr_tag){ //hit
                if(l2check)
                    l2hit=True;
                do_hit();
                if(RepPol==0)
                    TLB[TLB_TYPE][(addr_index*TLBc[TLB_TYPE].assoc)+i].count++;
                if(RepPol==1)
                    increase_LRU((addr_index*TLBc[TLB_TYPE].assoc)+i);
                return;
            }
            
        }//end for
        
        //Miss
        do_miss(t1,t2);
        
        if(l2check)
            l2hit=False;
        
        if(sim_L2TLB){
            Int save_tlb_type=TLB_TYPE;
            TLB_TYPE=TLB_TYPE_L2TLB;
            //we are now checking for L2 TLB.
            l2check=True;
            //avoid infinite loops
            sim_L2TLB=False;
            
            //tlb_lookup(addr_l2, addr_tag, addr_index, t1, t2);
            //call simulation again for L2, to possibly use different masks
            //and to save page if page tracking is on
            TLB_simulation(addr_l2,t1,t2);
            
            sim_L2TLB=True;
            //We no more check for L2 TLB
            l2check=False;
            TLB_TYPE=save_tlb_type;
        }

        if(sim_L2TLB==False || l2hit==False){
            if(RepPol==0){//LFU
                Int entry=LFU((Int)addr_index);
                TLB[TLB_TYPE][entry].tag=addr_tag;
                TLB[TLB_TYPE][entry].count=1;
            }
            if(RepPol==1){//LRU
                Int entry=LRU((Int)addr_index);
                // if(checkL2)
                //   VG_(umsg)("Entry:%d\n",entry);
                TLB[TLB_TYPE][entry].tag=addr_tag;
                TLB[TLB_TYPE][entry].count=0;
                increase_LRU(entry);
            }
            if(RepPol==2){//Random
                //generate a random number in the range 0-Nway assoc
                //and add it to the start of the set
                
                //calc start of the set
                Int base=addr_index*TLBc[TLB_TYPE].assoc;
                //calc random number in the rane 0-Nway
                Int entry=get_random(TLBc[TLB_TYPE].assoc);
                
                TLB[TLB_TYPE][base+entry].tag=addr_tag;
                TLB[TLB_TYPE][base+entry].count=0;
            }
        }
        
        return;
        
    }
    
}

/*----------------------------------------------------------------------------------------------*/

void TLB_simulation(Addr addr, ULong *t1, ULong *t2){
    
    
    /*
     Virtual Address Given:
     _________________________
     |      VPN       | OFFSET |
     -------------------------
     
     VPN:
     _________________________
     |     tag      | index    |
     -------------------------
     
     */
    
    //set these two to false before simulation
    l2hit=False;
    l2check=False;
    
    //Extract VPN from the virtual address
    Addr VPN=(addr & TLBc[TLB_TYPE].vpn_mask) >> TLBc[TLB_TYPE].offset_mask;
    //alternatively:
    //Addr VPN=addr/TLBc[TLB_TYPE].page_size;
    
    //1) Get Hits/Misses
    if(clo_sim_tlb||clo_sim_pages){
        
        //Direct mapped
        if(TLBc[TLB_TYPE].assoc==0){
            
            /* get index */
            // index contains only index.
            Addr index= VPN & TLBc[TLB_TYPE].index_mask;
            
            /* get tag */
            //VPN now contains tag and the index bits zeroed.
            VPN = VPN & TLBc[TLB_TYPE].tag_mask;
            //shift tag to get the real tag number without the index bits
            VPN = VPN >> log2(TLBc[TLB_TYPE].entries);
            
            if(clo_sim_tlb)
                tlb_lookup(addr,VPN,index,t1,t2);
        }//end dm if
        
        
        //Nway Associative
        if(TLBc[TLB_TYPE].assoc>0){
            
            /* Get index */
            //Only contains the index bits (i.e. the set number)
            Addr index = VPN & TLBc[TLB_TYPE].index_mask;
            
            /* get tag */
            //VPN now contains tag and index bits zeroed
            VPN = VPN & TLBc[TLB_TYPE].tag_mask;
            //shift tag to get the real tag number without the index bits
            VPN= VPN>>log2(TLBc[TLB_TYPE].index_mask+1);
            
            if(clo_sim_tlb)
                tlb_lookup(addr,VPN,index,t1,t2);
        }//end nway if
        
        
        //Fully Associative
        if(TLBc[TLB_TYPE].assoc==-1){
            //In fully associative, tag=VPN, thus pass it directly.
            if(clo_sim_tlb){
                tlb_lookup(addr,VPN,0,t1,t2);
                //tlb_lookupl2(VPN,t1,t2);
            }
            
        }//end fully assoc if
        
        
    }//end if
    
    /* 2) Save Pages */
    //VPN now contains the tag of the address
    if(clo_sim_pages)
        add_page(VPN);
    
    
    
    
}

/*----------------------------------------------------------------------------------------------*/


/* Int data_type takes the following values
 * 0 -> iTLB
 * 1 -> dTLB
 */

/* Intermediary Function */
void reference_address(Addr addr, Int data_type, ULong *t1, ULong *t2){
    
    if(data_type==TLB_TYPE_ITLB && sim_iTLB){
        TLB_TYPE=TLB_TYPE_ITLB;
        TLB_simulation(addr,t1,t2);
    }
    
    if(data_type==TLB_TYPE_DTLB && sim_dTLB){
        TLB_TYPE=TLB_TYPE_DTLB;
        TLB_simulation(addr,t1,t2);
    }
}

/*----------------------------------------------------------------------------------------------*/

void print_tlb(Int tlb){
    
    char *buf="";
    
    switch(tlb){
        case  0: buf="iTLB  (L1 Instruction TLB)";  break;
        case  1: buf="dTLB  (L1 Data TLB)";         break;
        default: buf="L2TLB (L2 Unified TLB)";      break;
    }
    
    VG_(umsg)("TLB type:          %s\n",buf);
    
    switch(TLBc[tlb].assoc){
        case -1: buf="Fully Associative";   break;
        case  0: buf="Direct Mapped";       break;
        default: buf="-Way Associative";    break;
    }
    
    if(TLBc[tlb].assoc>0)
        VG_(umsg)("Associativity:     %d%s\n",TLBc[tlb].assoc,buf);
    else
        VG_(umsg)("Associativity:     %s\n", buf);
    
    VG_(umsg)("Page Size:         %llu bytes\n", TLBc[tlb].page_size);
    VG_(umsg)("Entries:           %d\n", TLBc[tlb].entries);
    
    
    
}

/*----------------------------------------------------------------------------------------------*/

void tlb_chars(void){
    
    VG_(umsg)("\n\n\n---TLB characteristics---\n");
    VG_(umsg)("Virtual Address Size:     %d bits\n", VAS_SIZE);
    
    switch(RepPol){
        case 0: VG_(umsg)("Replacement Policy:       Least Frequently Used\n\n"); break;
        case 1: VG_(umsg)("Replacement Policy:       Least Recently Used\n\n"); break;
        case 2: VG_(umsg)("Replacement Policy:       Random\n\n"); break;
    }
    
    if(sim_iTLB){
        print_tlb(0);
        VG_(umsg)("\n\n");
    }
    if(sim_dTLB){
        print_tlb(1);
        VG_(umsg)("\n\n");
    }
    if(sim_L2TLB){
        print_tlb(2);
        VG_(umsg)("\n\n");
    }
    
    VG_(umsg)("\n---Results---\n\n");
}

/*----------------------------------------------------------------------------------------------*/

/* This function assumes that 'num' is in power of 2*/
/* Returns the power to which the number should be raised */
Int log2(Int num){
    
    Int log=0;
    while(num>1){
        num=num/2;
        log++;
    }
    //VG_(umsg)("log=%d",log);
    return log;
}


/*----------------------------------------------------------------------------------------------*/

ULong calc_VPN_MASK(Int offset, ULong page_size){
    Int i;
    ULong vpn_mask=1;
    //VG_(umsg)("offset=%d\n",offset);
    for(i=VAS_SIZE-offset;i>0;i--){
        vpn_mask=vpn_mask*2+1;
    }
    
    vpn_mask=vpn_mask*page_size;
    
    //VG_(umsg)("VPN_MASK=%lu\n",VPN_MASK);
    //VPN_MASK=total;
    return vpn_mask;
}

/*----------------------------------------------------------------------------------------------*/

static void init_tlb(Int tlb){
    
    //some easy initialisations first..
    TLBc[tlb].hit=0;
    TLBc[tlb].miss=0;
    //TLBc[tlb].page_size=4096;
    TLBc[tlb].replace=False;
    TLBc[tlb].page_ptr=NULL;
    TLBc[tlb].total_pages=0;
    TLBc[tlb].tlb_counter=0;
    
    switch(TLBc[tlb].assoc){
        case -1:
            VG_(sprintf)(TLBc[tlb].desc_line, "%llu B, %d E, Fully Associative",TLBc[tlb].page_size, TLBc[tlb].entries);
            break;
        case 0:
            VG_(sprintf)(TLBc[tlb].desc_line, "%llu B, %d E, Direct Mapped",TLBc[tlb].page_size, TLBc[tlb].entries);
            break;
        default:
            VG_(sprintf)(TLBc[tlb].desc_line, "%llu B, %d E, %d-wayAssociative",TLBc[tlb].page_size, TLBc[tlb].entries,TLBc[tlb].assoc);
            break;
    }
    
    //some more complicated now..
    
    //calculate offset and vpn masks
    TLBc[tlb].offset_mask=log2(TLBc[tlb].page_size);
    TLBc[tlb].vpn_mask=calc_VPN_MASK(TLBc[tlb].offset_mask,TLBc[tlb].page_size);
    
    Int i;
    switch(TLBc[tlb].assoc){
            
            //Fully Associative
        case -1:
            //In FA we don't need index mask nor tag_mask
            TLBc[tlb].index_mask=-1;
            TLBc[tlb].tag_mask=-1;
            break;
            
            
            //Direct Mapped
        case 0:
            //calculate index
            TLBc[tlb].index_mask=1;
            for(i=log2(TLBc[tlb].entries)-1;i>0;i--)
                TLBc[tlb].index_mask=TLBc[tlb].index_mask*2+1;
            
            //tag_mask is equal to 2^(VAS_SIZE-OFFSET_BITS-1)
            //e.g. 2^(32-12-1)=2^20 (not 2^19 because we shift bits!)
            TLBc[tlb].tag_mask=2<<(VAS_SIZE-TLBc[tlb].offset_mask-1);
            
            //Subtract index_mask from tag_mask
            TLBc[tlb].tag_mask=TLBc[tlb].tag_mask-TLBc[tlb].index_mask;
            break;
            
            
            //Nway Assoc
        default:
            //Nway can't be odd number
            tl_assert(TLBc[tlb].assoc%2==0);
            TLBc[tlb].sets=TLBc[tlb].entries/TLBc[tlb].assoc;
            //VG_(umsg)("Number of sets:%d\n",SETS);
            
            //index_mask acts as a SET MASK in this case
            TLBc[tlb].index_mask=1;
            for(i=log2(TLBc[tlb].sets)-1;i>0;i--)
                TLBc[tlb].index_mask=TLBc[tlb].index_mask*2+1;
            
            //VG_(umsg)("INDEX_MASK=%d",INDEX_MASK);
            TLBc[tlb].tag_mask=2<<(VAS_SIZE-TLBc[tlb].offset_mask-1);
            TLBc[tlb].tag_mask=TLBc[tlb].tag_mask-TLBc[tlb].index_mask;
            break;
            
    }
    
}


/*----------------------------------------------------------------------------------------------*/

void tlb_post_clo_init(void){
    
    TLB=(TLB_ENTRY **)VG_(malloc)("tlbg",3*sizeof(TLB_ENTRY*));
    
    //!!!IMPORTANT
    //init all TLBs so that we don't have to subtract to index e.g. if itlb is not initialised
    //then dtlb has to refer to [0] instead of [1] which messes up everything.
    TLB[0]=(TLB_ENTRY *)VG_(malloc)("itlb",0*sizeof(TLB_ENTRY));
    TLB[1]=(TLB_ENTRY *)VG_(malloc)("dtlb",0*sizeof(TLB_ENTRY));
    TLB[2]=(TLB_ENTRY *)VG_(malloc)("l2tlb",0*sizeof(TLB_ENTRY));
    
    Int i;
    //Initialise TLBs and allocate TLB arrays
    if(sim_iTLB){
        init_tlb(0);
        TLB[0]=(TLB_ENTRY *)VG_(malloc)("itlb",TLBc[0].entries*sizeof(TLB_ENTRY));
        //set count variable to 0
        for(i=0;i<TLBc[0].entries;i++)
            TLB[0][i].count=0;
    }
    if(sim_dTLB){
        init_tlb(1);
        TLB[1]=(TLB_ENTRY *)VG_(malloc)("dtlb",TLBc[1].entries*sizeof(TLB_ENTRY));
        //set count variable to 0
        for(i=0;i<TLBc[1].entries;i++)
            TLB[1][i].count=0;
    }
    if(sim_L2TLB){
        init_tlb(2);
        TLB[2]=(TLB_ENTRY *)VG_(malloc)("l2tlb",TLBc[2].entries*sizeof(TLB_ENTRY));
        //set count variable to 0
        for(i=0;i<TLBc[2].entries;i++)
            TLB[2][i].count=0;
    }
    
    
}


/*----------------------------------------------------------------------------------------------*/

void tlbsim_init(Int tlb_type, Int page_size, Int assoc, Int entries){
    
    //if page_size==-1, then TLB wasn't detected and thus don't do anything with it.
    //VG_(umsg)("tlb type: %d, page_size=%d\n",tlb_type,page_size);
    
    if(tlb_type==0 && page_size!=-1)
        sim_iTLB=True;
    if(tlb_type==1 && page_size!=-1)
        sim_dTLB=True;
    if(tlb_type==2 && page_size!=-1)
        sim_L2TLB=True;
    
    //if(page_size!=-1){
    TLBc[tlb_type].page_size  = page_size;
    TLBc[tlb_type].assoc      = assoc;
    TLBc[tlb_type].entries    = entries;
    //}
}

/*----------------------------------------------------------------------------------------------*/

void print_stats(Int hit, Int miss){
    VG_(umsg)("Total Accesses:   %d\n",hit+miss);
    VG_(umsg)("Hits:             %d\n",hit);
    VG_(umsg)("Misses:           %d\n",miss);
    
    static Char buf1[128];
    
    //percentify hits
    VG_(percentify)(hit, hit+miss, 1, 4, buf1);
    VG_(umsg)("Hit ratio:        %s\n",buf1);
    
    //percentify misses
    VG_(percentify)(miss, hit+miss, 1, 4,buf1);
    VG_(umsg)("Miss ratio:       %s\n",buf1);
    
    VG_(umsg)("\n\n");
}

/*----------------------------------------------------------------------------------------------*/

static void tlb_fini(void)
{
    
    tlb_chars();
    if(clo_sim_tlb){
        
        if(sim_iTLB){
            VG_(umsg)("---iTLB Stats---\n");
            print_stats(TLBc[0].hit,TLBc[0].miss);
        }
        
        if(sim_dTLB){
            VG_(umsg)("---dTLB Stats---\n");
            print_stats(TLBc[1].hit,TLBc[1].miss);
        }
        
        if(sim_L2TLB){
            VG_(umsg)("---L2TLB Stats---\n");
            print_stats(TLBc[2].hit,TLBc[2].miss);
        }
        
    }
    
    if(clo_sim_pages)
        print_pages();
    
    //TLB_TYPE=2;
    //print_tlb_contents();
    
    //free TLBs
    VG_(free)(TLB[0]);
    VG_(free)(TLB[1]);
    VG_(free)(TLB[2]);
    VG_(free)(TLB);
    
}

/*----------------------------------------------------------------------------------------------*/

/*--------------------------------------------------------------------*/
/*--- Command line processing                                      ---*/
/*--------------------------------------------------------------------*/

static Bool tlb_process_cmd_line_option(Char* arg)
{
    
    if      VG_BOOL_CLO(arg, "--tlb-sim"       , clo_sim_tlb  ) {}
    else if VG_BOOL_CLO(arg, "--tlb-page-sim"  , clo_sim_pages) {}
    else if VG_INT_CLO(arg,  "--tlb-vas-size"  , VAS_SIZE     ) {if(!(VAS_SIZE>0)) VG_(umsg)("Virtual Address Size has to be bigger than 0.\n");tl_assert(VAS_SIZE>0);}
    else if VG_INT_CLO(arg,  "--tlb-rep-pol"   , RepPol       ) {if(RepPol<0 || RepPol>2){ VG_(umsg)("Not valid replacement policy value. Setting to LRU.\n");RepPol=1; }}
    else
        return False;
    
    return True;
}

/*----------------------------------------------------------------------------------------------*/

static void tlb_print_usage(void)
{
    
    VG_(printf)(
                // "    TLB simulation Usage\n"
                "    --tlb-sim=yes|no       [yes]     collect TLB stats?\n"
                "    --tlb-page-sim=yes|no  [no]      collect pages used during TLB sim?\n"
                //  "    --enties=<num>         [64]      set TLB's number of entries\n"
                //  "    --assoc=<num>          [Ful Ass] set TLB's associativity, -1 for fully associative, 0 for direct mapped, N>0 for N-way\n"
                //  "    --page-size=<num>      [4096]    set TLB's page size (in bytes)\n"
                "    --tlb-vas-size=<num>   [32]      set TLB's virtual address space size (in bits)\n"
                "    --tlb-rep-pol=<num>    [1]       set TLB's Replacement Policy 0-> LFU, 1-> LRU, 2->Random\n"
                //  "    --type=<num>      [All]          set TLB's type. -1 for all, 0 for L1 iTLB, 1 for L1 dTLB, 2 for L2TLB\n"
                );
}

/*--------------------------------------------------------------------*/
/*--- end                                                cg_tlb.c ---*/
/*--------------------------------------------------------------------*/
