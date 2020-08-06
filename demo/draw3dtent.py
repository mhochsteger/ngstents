from ngstents import TentSlab
from netgen.csg import unit_cube
import ngsolve as ng
import numpy as np
import matplotlib.pyplot as plt

mesh = ng.Mesh(unit_cube.GenerateMesh(maxh=0.3))
print("creating tent pitched slab")
tps = TentSlab(mesh, dt=0.5, c=1.0)
ntents = tps.GetNTents()
print("The slab has {} tents.".format(ntents))
resp = ""
plt.ion()
while True:
    resp = input("Enter a tent number in 0:{} or q to quit: ".format(ntents))
    plt.close()
    if resp.lower() == 'q':
        break
    try:
        n = int(resp)
    except ValueError:
        continue
    ax = tps.Draw3DTentPlt(n)
    plt.show()



