import pyperclip

LEGACY = False

# py3 input
try:
    input = raw_input
except NameError:
    pass

def main():
	rawD = input("Paste cell count data here: ")
	d = rawD.split('=')[1][1:].split(' ')
	s = "tick\tliveCells\n" + "\n".join(d[i].split(':')[0] + '\t'+str(int(int(d[i].split(':')[1]) * (1.4011 if LEGACY else 1))) for i in range(len(d)))
	print(s)
	pyperclip.copy(s)

if __name__ == "__main__":
	main()