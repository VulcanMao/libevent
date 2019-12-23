#include <event2/event.h>


struct queue_entry_t{
	int value;

	TAILQ_ENTRY(queue_entry_t) entry;
};

TAILQ_HEAD(queue_head_t,queue_entry_t);

int main(int argc, cahr **argv){
	
	struct queue_head_t queue_head;
	struct queue_entry_t *q,*p,*s,*new_item;
	int i;

	TAILQ_INIT(&queue_head);

	for(i=0;i<3;++i){
		
		p = (struct queue_entry_t*)malloc(sizeof(struct queue_entry_t));
		p->value=i;

		TAILQ_INSERT_TAIL(&queue_head,p,entry);
	}
	return 0;
}
