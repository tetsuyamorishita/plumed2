/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2013-2016 The plumed team
   (see the PEOPLE file at the root of the distribution for a list of names)

   See http://www.plumed.org for more information.

   This file is part of plumed, version 2.

   plumed is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   plumed is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with plumed.  If not, see <http://www.gnu.org/licenses/>.
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++ */
#include "core/ActionRegister.h"
#include "VectorMultiColvar.h"

namespace PLMD {
namespace crystallization {

//+PLUMEDOC MCOLVAR MOLECULES
/*
Calculate the vectors connecting a pair of atoms in order to represent the orientation of a molecule.

At its simplest this command can be used to calculate the average length of an internal vector in a 
collection of different molecules.  When used in conjunction with MutiColvarFunctions in can be used
to do a variety of more complex tasks.

\par Examples

The following input tells plumed to calculate the distances between two of the atoms in a molecule.
This is done for the same set of atoms four different molecules and the average separation is then
calculated.

\verbatim
MOLECULES MOL1=1,2 MOL2=3,4 MOL3=5,6 MOL4=7,8 MEAN LABEL=mm
PRINT ARG=mm.mean FILE=colvar
\endverbatim


*/
//+ENDPLUMEDOC


class MoleculeOrientation : public VectorMultiColvar {
private:
  unsigned nvectors;
public:
  static void registerKeywords( Keywords& keys );
  explicit MoleculeOrientation( const ActionOptions& ao );
  void calculateVector( multicolvar::AtomValuePack& myatoms ) const;
};

PLUMED_REGISTER_ACTION(MoleculeOrientation,"MOLECULES")

void MoleculeOrientation::registerKeywords( Keywords& keys ){
  VectorMultiColvar::registerKeywords( keys ); keys.use("VMEAN");
  keys.add("numbered","MOL","The numerical indices of the atoms in the molecule. The orientation of the molecule is equal to " 
                            "the vector connecting the first two atoms specified.  If a third atom is specified its position "
                            "is used to specify where the molecule is.  If a third atom is not present the molecule is assumed "
                            "to be at the center of the vector connecting the first two atoms.");
  keys.reset_style("MOL","atoms");
}

MoleculeOrientation::MoleculeOrientation( const ActionOptions& ao ):
Action(ao),
VectorMultiColvar(ao)
{
  int natoms=-1; std::vector<AtomNumber> all_atoms;
  readAtomsLikeKeyword("MOL",natoms,all_atoms); 
  nvectors = std::floor( natoms / 2 );
  if( natoms%2!=0 && 2*nvectors+1!=natoms ) error("number of atoms in molecule specification is wrong.  Should be two or three.");

  if( all_atoms.size()==0 ) error("No atoms were specified");
  setVectorDimensionality( 3*nvectors ); setupMultiColvarBase( all_atoms );

  if( natoms==2*nvectors+1  ){
    std::vector<bool> catom_ind(natoms, false); catom_ind[natoms-1]=true;
    setAtomsForCentralAtom( catom_ind );
  } 
}

void MoleculeOrientation::calculateVector( multicolvar::AtomValuePack& myatoms ) const {
  for(unsigned i=0;i<nvectors;++i){
      Vector distance; distance=getSeparation( myatoms.getPosition(2*i+0), myatoms.getPosition(2*i+1) );

      addAtomDerivatives( 2+3*i+0, 0, Vector(-1.0,0,0), myatoms ); 
      addAtomDerivatives( 2+3*i+0, 1, Vector(+1.0,0,0), myatoms ); 
      myatoms.addBoxDerivatives( 2+3*i+0, Tensor(distance,Vector(-1.0,0,0)) ); 
      myatoms.addValue( 2+3*i+0, distance[0] ); 

      addAtomDerivatives( 2+3*i+1, 0, Vector(0,-1.0,0), myatoms ); 
      addAtomDerivatives( 2+3*i+1, 1, Vector(0,+1.0,0), myatoms ); 
      myatoms.addBoxDerivatives( 2+3*i+1, Tensor(distance,Vector(0,-1.0,0)) ); 
      myatoms.addValue( 2+3*i+1, distance[1] ); 

      addAtomDerivatives( 2+3*i+2, 0, Vector(0,0,-1.0), myatoms ); 
      addAtomDerivatives( 2+3*i+2, 1, Vector(0,0,+1.0), myatoms ); 
      myatoms.addBoxDerivatives( 2+3*i+2, Tensor(distance,Vector(0,0,-1.0)) ); 
      myatoms.addValue( 2+3*i+2, distance[2] );
  } 
}

}
}
