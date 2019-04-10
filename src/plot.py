import matplotlib.pyplot as plt
import os

hmNames = ["hm0.out","hm25.out","hm50.out","hm75.out"]
heatmapDir = cwd = os.getcwd().replace('\\','/')[:-4] + "/heatmaps attempt 3/final state only"

for fn in hmNames:
    print("loading {0}".format(fn))
    infile = open("{0}/{1}".format(heatmapDir,fn))
    data = [ [int(i) for i in line.strip().split(" ")] for line in infile]
    infile.close()
    heatmap = plt.pcolormesh(data,vmin=0, vmax = 540)
    cb = plt.colorbar(heatmap)
    plt.title(fn[2]+(fn[3] if fn[3].isdigit() else "")+"% Threshold for 128 Tick / 128 Node Run")
    cb.set_label('Number of Live Cells', rotation=90)
    plt.xlabel('Board Column')
    plt.ylabel('Board Row')
    plt.savefig(fn[:-4]+".png")
    cb.remove()
    print("finished {0}".format(fn))