#ifdef MDNS_HOST_TEST
#include "host.h"
#else
#include "target.h"
#endif
#include "mdns.h"

UINT16 mdns_read_n16( struct mdns_message *m )
{
	UINT16 n = ntohs(*((UINT16*)m->cur));
	m->cur += sizeof(UINT16);
	return n;
}

UINT32 mdns_read_n32( struct mdns_message *m )
{
	UINT32 n = ntohl(*((UINT32*)m->cur));
	m->cur += sizeof(UINT32);
	return n;
}

void mdns_write_n16( struct mdns_message *m, UINT16 n )
{
	*((UINT16*)m->cur) = htons(n);
	m->cur += sizeof(UINT16);
}

void mdns_write_n32( struct mdns_message *m, UINT32 n )
{
	*((UINT32*)m->cur) = htonl(n);
	m->cur += sizeof(UINT32);
}

/* a name may be one of:
 *  - a series of labels terminated by a NULL byte
 *  - a series of labels terminated by a pointer
 *  - a pointer */
void mdns_traverse_name( struct mdns_message *m )
{
	while( *(m->cur) != 0x00 ) {
		if( ( *(m->cur) & 0xC0 ) ) { /* pointer */
			m->cur++;
			break;
		}
		else /* label */
			m->cur += *(m->cur)+1;
	}
	m->cur++;
}

int mdns_parse_message( struct mdns_message *m, char *b )
{
	UINT16 i, t;

	m->header = (struct mdns_header *)b;
	m->num_questions = ntohs(m->header->qdcount);
	m->num_answers = ntohs(m->header->ancount);

	if( m->header->flags.opcode != 0 ) {
		DB_PRINT( "dropping message with opcode != 0\n" );
		return 0;
	}

	m->cur = (char *)m->header + sizeof(struct mdns_header);

	for( i = 0; i < m->num_questions; i++ ) {
		/* get qname */
		m->questions[i].qname = m->cur;
		mdns_traverse_name( m ); /* move past qname */
		/* get qtype */
		t = mdns_read_n16( m );
		if( t <= T_ANY )
			m->questions[i].qtype = t;
		else {
			DB_PRINT( "dropping message with invalid type %u\n", t );
			return 0;
		}
		/* get qclass */
		t = mdns_read_n16( m );
		if( ( t & ~0x8000 ) == 1 )
			m->questions[i].qclass = t;
		else {
			DB_PRINT( "dropping message with invalid class %u\n", t );
			return 0;
		}
	}

	for( i = 0; i < m->num_answers; i++ ) {
		m->answers[i].name = m->cur;
		mdns_traverse_name( m );
		m->answers[i].type = mdns_read_n16( m );
		m->answers[i].class = mdns_read_n16( m );
		m->answers[i].ttl = mdns_read_n32( m );
		m->answers[i].rdlength = mdns_read_n16( m );
		m->answers[i].rdata = (void *)m->cur;
	}
	return 1;	
}

/* transmit message creation */

void mdns_transmit_init( struct mdns_message *m, char *b )
{
	m->header = (struct mdns_header *)b;
	m->cur = b + sizeof(struct mdns_header);
	memset( m->header, 0x00, sizeof(struct mdns_header) );
}

void mdns_add_question( struct mdns_message *m, const char* qname, 
	UINT16 qtype, UINT16 qclass )
{
	strcpy( m->cur, qname ); /* copy name, including terminating null label*/
	m->cur += strlen( qname ) + 1;
	mdns_write_n16( m, qtype );
	mdns_write_n16( m, qclass );
	m->header->qdcount += htons(1);
}

void mdns_add_answer( struct mdns_message *m, const char *name, UINT16 type,
	UINT16 class, UINT32 ttl, UINT16 length, void *data )
{
	strcpy( m->cur, name );
	m->cur += strlen( name ) + 1;
	mdns_write_n16( m, type );
	mdns_write_n16( m, class );
	mdns_write_n32( m, ttl );
	mdns_write_n16( m, length );
	memcpy( m->cur, data, length );
	m->cur += length;
	m->header->ancount += htons(1);
}

/* debug printing */

void debug_print_name( char *name )
{
	UINT8 cur = 0;
	UINT16 ptr;
	char *s = name;
	
	while( *s ) {
		if( cur > 0 ) {
            putchar( *s );
            s++;
            cur--;
        }
        else {
            if( *s & 0xC0 ) { /* pointer */
                ptr = ((*s & ~(0xC0))<<8) | *(s+1);
                printf( "[PTR=0x%02X]", ptr );
                return;
            }
            else {
                putchar( '.' );
                cur = *s;
            }
            s++;
        }
	}
}

void debug_print_txt( char *txt, UINT16 len )
{
	UINT16 i;
	for( i = 0; i < len; i++ )
		putchar( txt[i] );
}

void debug_print_message( struct mdns_message *m )
{
	int i;

	printf( "printing message:\n" );

	printf( "--------------------------------------------------------\n"
			"header:\nID=%u, QR=%u, OPCODE=%u\n"
			"QDCOUNT=%u, ANCOUNT=%u, NSCOUNT=%u, ARCOUNT=%u\n",
			ntohs(m->header->id), m->header->flags.qr, m->header->flags.opcode,
			ntohs(m->header->qdcount), ntohs(m->header->ancount),
			ntohs(m->header->nscount), ntohs(m->header->arcount) );

	for( i = 0; i < m->num_questions; i++ ) {
		printf( "--------------------------------------------------------\n"
				"question %d: \"", i );
		debug_print_name( m->questions[i].qname );
		printf( "\" (type %u, class %u)\n", m->questions[i].qtype, 
				m->questions[i].qclass);
	}

	for( i = 0; i < m->num_answers; i++ ) {
		printf( "--------------------------------------------------------\n"
				"resource %d: \"", i );
		debug_print_name( m->answers[i].name );
		printf( "\" (type %u, class %u%s)\n\tttl=%u, rdlength=%u\n",
				m->answers[i].type, 
				m->answers[i].class & 0x8000 ? 
					m->answers[i].class & ~(0x8000) : m->answers[i].class, 
				m->answers[i].class & 0x8000 ? " FLUSH" : "", 
				m->answers[i].ttl,m->answers[i].rdlength );
		switch( m->answers[i].type ) {
			case T_A:
			printf( "\tA type, IP=0x%X\n", *((UINT32*)m->answers[i].rdata) );
			break;
			case T_NS:
			printf( "\tNS type, name=\"" );
			debug_print_name( (char *)m->answers[i].rdata );
			printf( "\"\n" );
			break;
			case T_CNAME:
			printf( "\tCNAME type, name=\"" ); 
			debug_print_name( (char *)m->answers[i].rdata );
			printf( "\"\n" );
			break;
			case T_SRV:
			printf( "\tSRV type, name=\"" );
			debug_print_name( (char *)m->answers[i].rdata );
			printf( "\"\n" );
			break;
			case T_PTR:
			printf( "\tPTR type, name=\"" );
			debug_print_name( (char *)m->answers[i].rdata );
			printf( "\"\n" );
			break;
			case T_TXT:
			printf( "\tTXT type, data=\"" ); 
			debug_print_txt( (char *)m->answers[i].rdata, 
				m->answers[i].rdlength );
			printf( "\"\n" );
			break;
			default:
			printf( "\tunknown RR type\n" );
			break;
		}
	}
}