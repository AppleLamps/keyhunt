#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "util.h"


char *ltrim(char *str, const char *seps)	{
	size_t totrim;
	if (seps == NULL) {
		seps = "\t\n\v\f\r ";
	}
	totrim = strspn(str, seps);
	if (totrim > 0) {
		size_t len = strlen(str);
		if (totrim == len) {
			str[0] = '\0';
		}
		else {
			memmove(str, str + totrim, len + 1 - totrim);
		}
	}
	return str;
}

char *rtrim(char *str, const char *seps)	{
	int i;
	if (seps == NULL) {
		seps = "\t\n\v\f\r ";
	}
	i = strlen(str) - 1;
	while (i >= 0 && strchr(seps, str[i]) != NULL) {
		str[i] = '\0';
		i--;
	}
	return str;
}

char *trim(char *str, const char *seps)	{
	return ltrim(rtrim(str, seps), seps);
}

int indexOf(char *s,const char **array,int length_array)	{
	int index = -1,i,continuar = 1;
	for(i = 0; i <length_array && continuar; i++)	{
		if(strcmp(s,array[i]) == 0)	{
			index = i;
			continuar = 0;
		}
	}
	return index;
}

char *nextToken(Tokenizer *t)	{
	if(t->current < t->n)	{
		t->current++;
		return t->tokens[t->current-1];
	}
	else {
		return  NULL;
	}
}
int hasMoreTokens(Tokenizer *t)	{
	return (t->current < t->n);
}

void stringtokenizer(char *data,Tokenizer *t)	{
	char *token;
	t->tokens = NULL;
	t->n = 0;
	t->current = 0;
	trim(data,"\t\n\r :");
	token = strtok(data," \t:");
	while(token != NULL)	{
		t->n++;
		t->tokens = (char**) realloc(t->tokens,sizeof(char*)*t->n);
		if(t->tokens == NULL)	{
			printf("Out of memory\n");
			exit(0);
		}
		t->tokens[t->n - 1] = token;
		token = strtok(NULL," \t:");
	}
}

void freetokenizer(Tokenizer *t)	{
	if(t->n > 0)	{
		free(t->tokens);
	}
	memset(t,0,sizeof(Tokenizer));
}


/*
	Aux function to get the hexvalues of the data
*/
char *tohex(char *ptr,int length){
  char *buffer;
  int offset = 0;
  unsigned char c;
  buffer = (char *) malloc((length * 2)+1);
  for (int i = 0; i <length; i++) {
    c = ptr[i];
	sprintf((char*) (buffer + offset),"%.2x",c);
	offset+=2;
  }
  buffer[length*2] = 0;
  return buffer;
}

void tohex_dst(char *ptr,int length,char *dst)	{
  int offset = 0;
  unsigned char c;
  for (int i = 0; i <length; i++) {
    c = ptr[i];
	sprintf((char*) (dst + offset),"%.2x",c);
	offset+=2;
  }
  dst[length*2] = 0;
}

// Hex digit value for each byte, -1 for anything that is not [0-9A-Fa-f]
static const struct HexTable	{
	signed char v[256];
	constexpr HexTable() : v()	{
		for(int i = 0; i < 256; i++)	{
			v[i] = (i >= '0' && i <= '9') ? i - '0' : (i >= 'A' && i <= 'F') ? i - 'A' + 10 : (i >= 'a' && i <= 'f') ? i - 'a' + 10 : -1;
		}
	}
} hextable;

int hexs2bin(char *hex, unsigned char *out)	{
	if (hex == NULL || *hex == '\0' || out == NULL)
		return 0;

	size_t len = strlen(hex);
	if (len % 2 != 0)
		return 0;
	len /= 2;

	memset(out, 'A', len);
	for (size_t i = 0; i < len; i++) {
		int b1 = hextable.v[(unsigned char)hex[i*2]];
		int b2 = hextable.v[(unsigned char)hex[i*2+1]];
		if (b1 < 0 || b2 < 0) {
			return 0;
		}
		out[i] = (unsigned char)((b1 << 4) | b2);
	}
	return (int)len;
}

int hexchr2bin(const char hex, char *out)	{
	if (out == NULL)
		return 0;
	int v = hextable.v[(unsigned char)hex];
	if (v < 0)
		return 0;
	*out = (char)v;
	return 1;
}

void addItemList(char *data, List *l)	{
	l->data = (char**) realloc(l->data,sizeof(char*)* (l->n +1));
	l->data[l->n] = data;
	l->n++;
}

int isValidHex(char *data)	{
	for(const unsigned char *c = (const unsigned char*)data; *c; c++)	{
		if(hextable.v[*c] < 0)
			return 0;
	}
	return 1;
}
