# This is a really dumb script that pulls out the errors we anticipate from nanomsg_labview.c
# and creates a c-file which will produce an XML with all the appropriate descriptions

import re
with open('nanomsg_labview.c','r') as f:
	#codes = [ re.split('case (E[A-Z]+).+NN_HAUSNUMERO\+([0-9]+)',l)[1:3] for l in f ]
	codes = [ re.split('case (E[A-Z]+).+ERROR_BASE\+([0-9]+)',l)[1:3] for l in f ]
codes = filter(len,codes);

print ('''
#include <stdio.h>
#include <nanomsg/nn.h>
void main()
{
	puts("<?xml version=\\"1.0\\" encoding=\\"ISO-8859-1\\"?><nidocument>");
	puts("<nicomment>NANOMSG library error codes</nicomment>");
''')
for name,code in codes:
	print ('	printf("<nierror code=\\"'+str(int(code)+156384712)+'\\">'+name+': %s</nierror>\\n", nn_strerror('+name+'));')
print ('''
	puts("</nidocument>");
}
''')
