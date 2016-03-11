// *******************************************************************************
// Multi-level PDFCM Adaptive Prefetching
// based on Performance Gradient Tracking
// BASIC RELEASE
// Ramos, Briz, Ibanez, Vinals
// *******************************************************************************

// Prefetch bit / tag - request by demand of "intelligence"?
// PDFCM : Prefetch differential finite context machine
// PMAF: Prefetch Memory Address File
// MSHR and PMAF are both FIFO structures

#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include "interface.hh"


#define PDFCM_HT_bits (8)
#define PDFCM_HT_size (1 << PDFCM_HT_bits)
#define PDFCM_HT_mask (PDFCM_HT_size - 1)
#define PDFCM_DT_bits (9)
#define PDFCM_DT_size (1 << PDFCM_DT_bits)
#define PDFCM_DT_mask (PDFCM_DT_size - 1)

typedef struct PDFCM_HT_entry PDFCM_HT_entry;
struct PDFCM_HT_entry
{
    unsigned short
    PC;      // address of the memory access instruction         (16 bits)
    unsigned short
    last_addr;       // last address that missed the cache               (16 bits)
    unsigned short history;     // hashed history of deltas                 (9 bits)
    char count;         // Confidence Tick                       (2 bits)
};
PDFCM_HT_entry *
PDFCM_HT;      //                              (256 * 43 = 10,75 Kbits)

typedef struct PDFCM_DT_entry PDFCM_DT_entry;
struct PDFCM_DT_entry
{
    short delta;            // delta between consecutive addresses              (16 bits)
};
PDFCM_DT_entry *
PDFCM_DT;      //                              (512 * 16 = 8 Kbits)

void PDFCM_ini (void);
Addr PDFCM_update_and_predict (Addr pc, Addr addr,
                               unsigned short
                               *history);       // updates HT & DT and predicts the next address that is going to miss
Addr PDFCM_predict_next (Addr addr, unsigned short *history,
                         unsigned short last_addr);   // used by the PDA to predict the next addresses
unsigned short PDFCM_hash (unsigned short old_history,
                           short delta);                    // calculates the new hashed history of deltas
void PDFCM_cycle(AccessStat *L2Data);

// PDFCM Degree Automaton (PDA)
unsigned short PDA_history;
Addr PDA_addr;
unsigned short PDA_last_addr;
char PDA_degree=0;


void SEQTL1_cycle(AccessStat *L1Data);
// SEQT L1 Degree Automaton (SDA1)
Addr SeqDegreeAuto_lastaddr;
char SeqDegreeAuto_degree=0;

typedef struct MSHR_entry MSHR_entry;
struct MSHR_entry
{
    Addr addr;
    char valid;
};
typedef struct MSHR MSHR;
struct MSHR
{
    int size;
    int tail;
    int num;
    int head;
    MSHR_entry entry[32];
};
MSHR * PMAF1;
MSHR * MSHRD2;
MSHR * PMAF2;

void MSHR_ini (void);
int MSHR_lookup (MSHR * MSHR, Addr addr);     // if hit it returns 1
void MSHR_insert (MSHR * MSHR,
                  Addr addr);    // if full, the oldest entry is deleted
void MSHR_cycle ();                // it deletes the oldest entry if it hits on the cache (using GetPrefetchBit)

unsigned int AdaptiveDegree_Cycles=0;
Tick AD_L1_accesses=0;
Tick AD_last_L1_accesses=0;
#define AD_interval (64*1024)
#define AD_MAX_INDEX (13)   // number of different degrees
int AD_degs[AD_MAX_INDEX]= {0,1,2,3,4,6,8,12,16,24,32,48,64};
int AD_deg_index=4;
int AD_degree=4;
char AD_state=0; // 0: increasing degree;  1: decreasing degree

void AdaptiveDegree_Cycle(AccessStat *L1Data);

void PDFCM_ini (void)
{
    PDFCM_HT=(PDFCM_HT_entry *) calloc(PDFCM_HT_size, sizeof(PDFCM_HT_entry));
    PDFCM_DT=(PDFCM_DT_entry *) calloc(PDFCM_DT_size, sizeof(PDFCM_DT_entry));
}

#define MASK_16b (0xffff)
Addr PDFCM_update_and_predict (Addr pc, Addr addr, unsigned short *history)
{
    unsigned short new_last_addr;
    unsigned short new_history;
    short actual_delta, predicted_delta;
    // read PDFCM_HT entry
    unsigned int index = pc & PDFCM_HT_mask;
    unsigned short old_last_addr = PDFCM_HT[index].last_addr;
    unsigned short old_history = PDFCM_HT[index].history;
    char count = PDFCM_HT[index].count;
    if (PDFCM_HT[index].PC!=((pc>>PDFCM_HT_bits) & MASK_16b))
    {
        // if it's a new PC replace the entry
        PDFCM_HT[index].PC=((pc>>PDFCM_HT_bits) & MASK_16b);
        PDFCM_HT[index].history=0;
        PDFCM_HT[index].last_addr=addr & MASK_16b;
        PDFCM_HT[index].count=0;
        return 0;
    }
    // compute deltas & update confidence Tick
    predicted_delta=PDFCM_DT[old_history].delta;
    actual_delta = (addr-old_last_addr) & MASK_16b;
    if (actual_delta==predicted_delta)
    {
        if (count<3) { count++; }
    }
    else {
        if (count>0) { count--; }
    }
    // compute new history
    new_last_addr = addr & MASK_16b;
    *history   = new_history   = PDFCM_hash(old_history, actual_delta);
    // write PDFCM_HT entry
    PDFCM_HT[index].last_addr = new_last_addr;
    PDFCM_HT[index].history   = new_history;
    PDFCM_HT[index].count=count;
    // update PDFCM_DT entry
    PDFCM_DT[old_history].delta = actual_delta;
    // predict a new delta using the new history
    if (count<2)
        return 0;
    else
        return addr + PDFCM_DT[new_history].delta;
}

Addr PDFCM_predict_next (Addr addr, unsigned short *history,
                         unsigned short old_last_addr)
{
    short delta;
    // compute delta
    delta = (addr-old_last_addr) & MASK_16b;
    // compute new history
    *history=PDFCM_hash(*history, delta);
    // predict a new delta using the new history
    return addr + PDFCM_DT[*history].delta;
}

unsigned short PDFCM_hash (unsigned short old_history, short delta)
{
    // R5-->  R(16, 16, 16)  F(t, t, t)  S(10, 5, 0) ; (number of entries of PDFCM_DT = 2^t)
    unsigned short select, folded, shift;
    select = delta;
    for(folded=0; select;)
    {
        folded ^= select & PDFCM_DT_mask;  // fold t bits
        select= select >> PDFCM_DT_bits;
    }
    shift = (old_history << 5) & PDFCM_DT_mask;     // shift 5 bits
    return shift ^ folded;
}

void MSHR_ini (void)
{
    PMAF1=(MSHR *) calloc(1, sizeof(MSHR));
    PMAF1->size=32;
    MSHRD2=(MSHR *) calloc(1, sizeof(MSHR));
    MSHRD2->size=16;
    PMAF2=(MSHR *) calloc(1, sizeof(MSHR));
    PMAF2->size=32;
}
int MSHR_lookup (MSHR * MSHR, Addr addr)
{
    int i;
    Addr MASK = 0x3f;
    if (!MSHR->size || !MSHR->num)
        return 0;
    for (i=0; i < MSHR->size; i++)
        if (MSHR->entry[i].valid && (MSHR->entry[i].addr == (addr & ~MASK)) )
            return 1;
    return 0;
}
void MSHR_insert (MSHR * MSHR, Addr addr)
{
    Addr MASK = 0x3f;
    if (!MSHR->size || !addr)
        return;
    MSHR->entry[MSHR->tail].valid=1;
    MSHR->entry[MSHR->tail].addr= (addr & ~MASK);
    MSHR->tail=(MSHR->tail+1)%MSHR->size;
    if (MSHR->num<MSHR->size)
        MSHR->num++;
    else
        MSHR->head=MSHR->tail;
}

void MSHR_cycle ()
{
    if (PMAF1->num && (get_prefetch_bit(PMAF1->entry[PMAF1->head].addr) != 0 ))
    {
        PMAF1->num--;
        PMAF1->entry[PMAF1->head].valid=0;
        PMAF1->head=(PMAF1->head+1)%PMAF1->size;
    }
    if (MSHRD2->num && (get_prefetch_bit(MSHRD2->entry[MSHRD2->head].addr) != 0) )
    {
        MSHRD2->num--;
        MSHRD2->entry[MSHRD2->head].valid=0;
        MSHRD2->head=(MSHRD2->head+1)%MSHRD2->size;
    }
    if (PMAF2->num && (get_prefetch_bit(PMAF2->entry[PMAF2->head].addr) != 0) )
    {
        PMAF2->num--;
        PMAF2->entry[PMAF2->head].valid=0;
        PMAF2->head=(PMAF2->head+1)%PMAF2->size;
    }
}

#define LOOKBACK_DEGREE 4
struct AccessStat accessStatHistory[LOOKBACK_DEGREE];

void AdaptiveDegree_Cycle(AccessStat *L1Data)
{
    int i;
    AdaptiveDegree_Cycles++;
    // Count how many references there were to L1 in the previous cycle
    // Optimally, we would have an instruction counter, but sim framework is limited
    // We use AD_L1_access to determine L2 adaption degree
    for(i = 0; i < LOOKBACK_DEGREE; i++)
        if(L1Data->time == accessStatHistory[i].time)
            AD_L1_accesses++;

    // AD_interval is an epoch (given number of cycles). When we pass this,
    // we change our degree automaton to either increasing or decreasing (AD_STATE)
    // In an epoch, we define success on whether or not we have many L1 hits
    // See page 7 in ./doc/doc.pdf
    if (AdaptiveDegree_Cycles>AD_interval)
    {
        AdaptiveDegree_Cycles=0;
        if (AD_L1_accesses<AD_last_L1_accesses)
            AD_state=!AD_state;
        if (!AD_state)
        {
            if (AD_deg_index<AD_MAX_INDEX-1) 
                AD_deg_index++;
        }
        else
            if (AD_deg_index>0) 
                AD_deg_index--;
        AD_degree=AD_degs[AD_deg_index];
        AD_last_L1_accesses=AD_L1_accesses;
        AD_L1_accesses=0;
    }
}
// *******************************************************************************
//          SEQTL1 (L1 Prefetcher)
// *******************************************************************************
void SEQTL1_cycle(AccessStat *L1Data)
{
    int i;
    for (i=0; i<LOOKBACK_DEGREE; i++)
    {
        // miss or "1st use" in L1?
        if (L1Data->time == accessStatHistory[i].time && (accessStatHistory[i].miss == 1 ||
                get_prefetch_bit(accessStatHistory[i].mem_addr)==1) )
        {
            if (accessStatHistory[i].miss == 0)
                clear_prefetch_bit(accessStatHistory[i].mem_addr);
            // program SDA1
            SeqDegreeAuto_lastaddr=accessStatHistory[i].mem_addr;
            SeqDegreeAuto_degree=(accessStatHistory[i].miss == 0 ? 1 : 4 );
        } 
    } 
    if (SeqDegreeAuto_degree)
    {
        Addr predicted_address= SeqDegreeAuto_lastaddr+0x40;
        // issue prefetch (if not filtered)
        if (get_prefetch_bit(predicted_address) == 0
        && !MSHR_lookup(PMAF1,predicted_address & 0xffff))
        {
            if(predicted_address < MAX_PHYS_MEM_ADDR )
            {
                issue_prefetch(predicted_address);
                set_prefetch_bit(predicted_address);
                MSHR_insert(PMAF1,predicted_address & 0xffff);
            }
        }
        // program next prefetch for the next cycle
        SeqDegreeAuto_lastaddr=predicted_address;
        SeqDegreeAuto_degree--;
    }
}
void PDFCM_cycle(AccessStat *L2Data)
{
    Addr predicted_address;
    unsigned short history=PDA_history;
    // miss or "1st use" in L2? (considering only demand references)
    if (/*L2Data->tim == L2Data->time && !L2Data->LastRequestPrefetch && */
            ((L2Data->miss == 1  && !MSHR_lookup(MSHRD2, L2Data->mem_addr)) ||
             get_prefetch_bit(L2Data->mem_addr)==1)  )
    {
        if (L2Data->miss == 1)
            MSHR_insert(MSHRD2, L2Data->mem_addr);
        else
            clear_prefetch_bit(L2Data->mem_addr);
        // update PDFCM tables, predict next missing address and get the new history
        predicted_address = PDFCM_update_and_predict(L2Data->pc,
                            L2Data->mem_addr, &history);
        if (AD_degree)
        {
            // issue prefetch (if not filtered)
            if (predicted_address && predicted_address!=L2Data->mem_addr &&
                    get_prefetch_bit(predicted_address)==0 )
                if (!MSHR_lookup(PMAF2,predicted_address & 0xffff) &&
                        !MSHR_lookup(MSHRD2, predicted_address))
                {
                    if(predicted_address < MAX_PHYS_MEM_ADDR )
                    {
                        issue_prefetch(predicted_address);
                        set_prefetch_bit(predicted_address);
                        MSHR_insert(PMAF2,predicted_address & 0xffff);
                    }
                }
            if (predicted_address)
            {
                // program PDA
                PDA_history=history;
                PDA_last_addr=L2Data->mem_addr & MASK_16b;
                PDA_addr=predicted_address;
                PDA_degree=AD_degree-1;
            }
        }
        // else the PDA generates 1 prefetch per cycle
    }
    else if (PDA_degree && PDA_history)
    {
        // predict next missing address and get the new history
        predicted_address = PDFCM_predict_next(PDA_addr, &PDA_history, PDA_last_addr);
        // issue prefetch (if not filtered)
        if (predicted_address && predicted_address!=PDA_addr &&
                get_prefetch_bit(predicted_address)==0 )
        {
            if (!MSHR_lookup(PMAF2,predicted_address & 0xffff) &&
                    !MSHR_lookup(MSHRD2,predicted_address))
            {
                if(predicted_address < MAX_PHYS_MEM_ADDR )
                {
                    issue_prefetch(predicted_address);
                    set_prefetch_bit(predicted_address);
                    MSHR_insert(PMAF2,predicted_address & 0xffff);
                }
            }
        }
        // program PDA
        PDA_last_addr=PDA_addr & MASK_16b;
        PDA_addr=predicted_address;
        PDA_degree--;
    }
}
void prefetch_init(void)
{
    /* Called before any calls to prefetch_access. */
    /* This is the place to initialize data structures. */
    PDFCM_ini();
    MSHR_ini();
    struct AccessStat *tmp = (struct AccessStat*)malloc(sizeof(struct AccessStat));
    tmp->time = -1;
    tmp->pc = -1;
    tmp->mem_addr = -1;
    tmp->miss = -1;
    for(int i = 0; i < LOOKBACK_DEGREE; i++)
        accessStatHistory[i] = *tmp;
}

int index = 0;
void prefetch_access(AccessStat stat)
{
    //  Function that is called every cycle to issue prefetches should the
    //  prefetcher want to.  The arguments to the function are the current cycle,
    //  the demand requests to L1 and L2 cache.
    MSHR_cycle();
    AccessStat *ptr = &stat;
    AdaptiveDegree_Cycle(ptr);
    SEQTL1_cycle(ptr);
    PDFCM_cycle(ptr);

    accessStatHistory[(index++)%4] = stat;
}

void prefetch_complete(Addr addr)
{
    /* Called when a block requested by the prefetcher has been loaded.
     */
}
