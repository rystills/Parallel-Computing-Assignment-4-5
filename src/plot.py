import matplotlib.pyplot as plt

infile = open("heatmap.out")
data = [ [int(i) for i in line.split(" ")] for line in infile ]
infile.close()

plt.pcolormesh(data);
plt.savefig("heatmap.png");
