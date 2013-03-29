/* +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
   Copyright (c) 2012 The plumed team
   (see the PEOPLE file at the root of the distribution for a list of names)

   See http://www.plumed-code.org for more information.

   This file is part of plumed, version 2.0.

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
#include "MultiColvar.h"
#include "core/PlumedMain.h"
#include "core/ActionSet.h"
#include "core/SetupMolInfo.h"
#include "vesselbase/Vessel.h"
#include "tools/Pbc.h"
#include <vector>
#include <string>

using namespace std;
namespace PLMD{
namespace multicolvar{

void MultiColvar::registerKeywords( Keywords& keys ){
  Action::registerKeywords( keys );
  ActionWithValue::registerKeywords( keys );
  ActionAtomistic::registerKeywords( keys );
  keys.addFlag("NOPBC",false,"ignore the periodic boundary conditions when calculating distances");
  keys.reserve("numbered","ATOMS","the atoms involved in each of the collective variables you wish to calculate. "
                               "Keywords like ATOMS1, ATOMS2, ATOMS3,... should be listed and one CV will be "
                               "calculated for each ATOM keyword you specify (all ATOM keywords should "
                               "define the same number of atoms).  The eventual number of quantities calculated by this "
                               "action will depend on what functions of the distribution you choose to calculate."); 
  keys.reset_style("ATOMS","atoms");
  keys.reserve("atoms-3","SPECIES","this keyword is used for colvars such as coordination number. In that context it specifies that plumed should calculate "
                                 "one coordination number for each of the atoms specified.  Each of these coordination numbers specifies how many of the "
                                 "other specified atoms are within a certain cutoff of the central atom.");
  keys.reserve("atoms-4","SPECIESA","this keyword is used for colvars such as the coordination number.  In that context it species that plumed should calculate "
                                  "one coordination number for each of the atoms specified in SPECIESA.  Each of these cooordination numbers specifies how many "
                                  "of the atoms specifies using SPECIESB is within the specified cutoff");
  keys.reserve("atoms-4","SPECIESB","this keyword is used for colvars such as the coordination number.  It must appear with SPECIESA.  For a full explanation see " 
                                  "the documentation for that keyword");
  keys.addFlag("VERBOSE",false,"write a more detailed output");
  ActionWithVessel::registerKeywords( keys );
  keys.use("NL_TOL");
  keys.add("hidden","NL_STRIDE","the frequency with which the neighbor list should be updated. Between neighbour list update steps all quantities "
                                "that contributed less than TOL at the previous neighbor list update step are ignored.");
} 

MultiColvar::MultiColvar(const ActionOptions&ao):
Action(ao),
ActionAtomistic(ao),
ActionWithValue(ao),
ActionWithVessel(ao),
usepbc(false),
readatoms(false),
verbose_output(false),
updateFreq(0),
lastUpdate(0),
posHasBeenSet(false),
centralAtomDerivativesAreInFractional(false)
{
  if( keywords.exists("NOPBC") ){ 
    bool nopbc=!usepbc; parseFlag("NOPBC",nopbc);
    usepbc=!nopbc;
  } 
  parseFlag("VERBOSE",verbose_output);
  if( keywords.exists("NL_STRIDE") ) parse("NL_STRIDE",updateFreq);
  if(updateFreq>0) log.printf("  Updating contributors every %d steps.\n",updateFreq);
  else log.printf("  Updating contributors every step.\n");
}

void MultiColvar::addColvar( const std::vector<unsigned>& newatoms ){
  if( colvar_atoms.size()>0 ) plumed_assert( colvar_atoms[0].fullSize()==newatoms.size() );
  DynamicList<unsigned> newlist;
  if( verbose_output ) log.printf("  Colvar %d is calculated from atoms : ", colvar_atoms.size()+1);
  for(unsigned i=0;i<newatoms.size();++i){
     if( verbose_output ) log.printf("%d ",all_atoms(newatoms[i]).serial() );
     newlist.addIndexToList( newatoms[i] );
  }
  if( verbose_output ) log.printf("\n");
  taskList.addIndexToList( colvar_atoms.size() );
  colvar_atoms.push_back( newlist );
} 

void MultiColvar::readAtoms( int& natoms ){
  if( keywords.exists("ATOMS") ) readAtomsLikeKeyword( "ATOMS", natoms );
  if( keywords.exists("GROUP") ) readGroupsKeyword( natoms );
  if( keywords.exists("SPECIES") ) readSpeciesKeyword( natoms );

  if( !readatoms ) error("No atoms have been read in");
  taskList.activateAll();
  for(unsigned i=0;i<colvar_atoms.size();++i) colvar_atoms[i].activateAll(); 
  // Set up dynamic array for derivatives
  for(unsigned i=0;i<colvar_atoms[0].fullSize();++i) atoms_with_derivatives.addIndexToList( i );
  // Set up stuff for central atoms
  for(unsigned i=0;i<colvar_atoms[0].fullSize();++i) atomsWithCatomDer.addIndexToList( i );
  atomsWithCatomDer.deactivateAll();
  central_derivs.resize( colvar_atoms[0].fullSize() );
  for(unsigned i=0;i<central_derivs.size();++i) central_derivs[i].zero();
  // Resize all dynamic content
  resizeDynamicArrays();
}

void MultiColvar::readBackboneAtoms( const std::vector<std::string>& backnames, std::vector<unsigned>& chain_lengths ){
  plumed_massert( !readatoms, "I can only read atons using the RESIDUES keyword" );
  plumed_massert( keywords.exists("RESIDUES"), "To read in the backbone atoms the keyword RESIDUES must be registered");
  readatoms=true;

  std::vector<SetupMolInfo*> moldat=plumed.getActionSet().select<SetupMolInfo*>();
  if( moldat.size()==0 ) error("Unable to find MOLINFO in input");

  std::vector<std::string> resstrings; parseVector( "RESIDUES", resstrings );
  if( !verbose_output ){
      if(resstrings[0]=="all"){
         log.printf("  examining all possible secondary structure combinations");
      } else {
         log.printf("  examining secondary struture in residue poritions : %s ",resstrings[0].c_str() );
         for(unsigned i=1;i<resstrings.size();++i) log.printf(", %s",resstrings[1].c_str() );
         log.printf("\n");
      }
  }
  std::vector< std::vector<AtomNumber> > backatoms; 
  moldat[0]->getBackbone( resstrings, backnames, backatoms );

  chain_lengths.resize( backatoms.size() );
  for(unsigned i=0;i<backatoms.size();++i){
     chain_lengths[i]=backatoms[i].size();
     for(unsigned j=0;j<backatoms[i].size();++j) all_atoms.addIndexToList( backatoms[i][j] );
  }
}

void MultiColvar::readAtomsLikeKeyword( const std::string key, int& natoms ){ 
  if( readatoms) return; 

  std::vector<AtomNumber> t; std::vector<unsigned> newlist;
  for(int i=1;;++i ){
     parseAtomList(key, i, t );
     if( t.empty() ) break;

     if(!verbose_output){
        log.printf("  Colvar %d is calculated from atoms : ", i);
        for(unsigned j=0;j<t.size();++j) log.printf("%d ",t[j].serial() );
        log.printf("\n"); 
     }

     if( i==1 && natoms<0 ) natoms=t.size();
     if( t.size()!=natoms ){
         std::string ss; Tools::convert(i,ss); 
         error(key + ss + " keyword has the wrong number of atoms"); 
     }
     for(unsigned j=0;j<natoms;++j){ 
        newlist.push_back( natoms*(i-1)+j ); 
        all_atoms.addIndexToList( t[j] );
     }
     t.resize(0); addColvar( newlist );
     newlist.clear(); readatoms=true;
  }
}

void MultiColvar::readGroupsKeyword( int& natoms ){
  if( readatoms ) return;

  if( natoms==2 ){
      if( !keywords.exists("GROUPA") ) error("use GROUPA and GROUPB keywords as well as GROUP");
      if( !keywords.exists("GROUPB") ) error("use GROUPA and GROUPB keywords as well as GROUP");
  } else if( natoms==3 ){
      if( !keywords.exists("GROUPA") ) error("use GROUPA, GROUPB and GROUPC keywords as well as GROUP");
      if( !keywords.exists("GROUPB") ) error("use GROUPA, GROUPB and GROUPC keywords as well as GROUP");
      if( !keywords.exists("GROUPC") ) error("use GROUPA, GROUPB and GROUPC keywords as well as GROUP");
  } else {
      error("Cannot use groups keyword unless the number of atoms equals 2");
  }
  
  std::vector<AtomNumber> t;
  parseAtomList("GROUP",t);
  if( !t.empty() ){
      readatoms=true;
      for(unsigned i=0;i<t.size();++i) all_atoms.addIndexToList( t[i] );
      std::vector<unsigned> newlist;
      if(natoms==2){ 
         for(unsigned i=1;i<t.size();++i){ 
             for(unsigned j=0;j<i;++j){ 
                newlist.push_back(i); newlist.push_back(j); addColvar( newlist ); newlist.clear();
             }
         }
      } else if(natoms==3){
         for(unsigned i=2;i<t.size();++i){
            for(unsigned j=1;j<i;++j){
               for(unsigned k=0;k<j;++k){
                   newlist.push_back(i); newlist.push_back(j);
                   newlist.push_back(k); addColvar( newlist );
                   newlist.clear();
               }
            }
         }
      }
      if( !verbose_output ){
          log.printf("  constructing colvars from %d atoms : ", t.size() );
          for(unsigned i=0;i<t.size();++i) log.printf("%d ",t[i].serial() );
          log.printf("\n");
      }
  } else {
      std::vector<AtomNumber> t1,t2; 
      parseAtomList("GROUPA",t1);
      if( !t1.empty() ){
         readatoms=true;
         parseAtomList("GROUPB",t2);
         if ( t2.empty() && natoms==2 ) error("GROUPB keyword defines no atoms or is missing. Use either GROUPA and GROUPB or just GROUP"); 
         for(unsigned i=0;i<t1.size();++i) all_atoms.addIndexToList( t1[i] ); 
         for(unsigned i=0;i<t2.size();++i) all_atoms.addIndexToList( t2[i] ); 
         std::vector<unsigned> newlist;
         if(natoms==2){
            for(unsigned i=0;i<t1.size();++i){
                for(unsigned j=0;j<t2.size();++j){
                    newlist.push_back(i); newlist.push_back( t1.size() + j ); addColvar( newlist ); newlist.clear();
                }
            }
         } else if(natoms==3){
            if ( t2.empty() ) error("GROUPB keyword defines no atoms or is missing. Use either GROUPA and GROUPB, GROUPA, GROUPB and GROUPC or just GROUP");  
            std::vector<AtomNumber> t3;
            parseAtomList("GROUPC",t3);
            if( t3.empty() ){
                for(unsigned i=0;i<t1.size();++i){
                   for(unsigned j=1;j<t2.size();++j){ 
                      for(unsigned k=0;k<j;++k){
                           newlist.push_back( t1.size() + j );
                           newlist.push_back(i);
                           newlist.push_back( t1.size() + k );
                           addColvar( newlist ); newlist.clear();
                      }
                   }
                }
            } else {
                for(unsigned i=0;i<t3.size();++i) all_atoms.addIndexToList( t3[i] );
                for(unsigned i=0;i<t1.size();++i){
                    for(unsigned j=0;j<t2.size();++j){
                        for(unsigned k=0;k<t3.size();++k){
                           newlist.push_back( t1.size() + j );
                           newlist.push_back(i); 
                           newlist.push_back( t1.size() + t2.size() + k );
                           addColvar( newlist ); newlist.clear();
                        }
                    }
                }
            }
         }
      }
      if( !verbose_output ){
          log.printf("  constructing colvars from two groups containing %d and %d atoms respectively\n",t1.size(),t2.size() );
          log.printf("  group A contains atoms : ");
          for(unsigned i=0;i<t1.size();++i) log.printf("%d ",t1[i].serial() );
          log.printf("\n"); 
          log.printf("  group B contains atoms : ");
          for(unsigned i=0;i<t2.size();++i) log.printf("%d ",t2[i].serial() );
          log.printf("\n");
      }
  }
}

void MultiColvar::readSpeciesKeyword( int& natoms ){
  if( readatoms ) return ;

  std::vector<AtomNumber> t;
  parseAtomList("SPECIES",t);
  if( !t.empty() ){
      readatoms=true; natoms=t.size();
      for(unsigned i=0;i<t.size();++i) all_atoms.addIndexToList( t[i] );
      std::vector<unsigned> newlist;
      if( keywords.exists("SPECIESA") && keywords.exists("SPECIESB") ){
          for(unsigned i=0;i<t.size();++i){
              newlist.push_back(i);
              for(unsigned j=0;j<t.size();++j){
                  if(i!=j) newlist.push_back(j); 
              }
              addColvar( newlist ); newlist.clear();
          }
          if( !verbose_output ){
              log.printf("  generating colvars from %d atoms of a particular type\n",t.size() );
              log.printf("  atoms involved : "); 
              for(unsigned i=0;i<t.size();++i) log.printf("%d ",t[i].serial() );
              log.printf("\n");
          }
      } else if( !( keywords.exists("SPECIESA") && keywords.exists("SPECIESB") ) ){
          std::vector<unsigned> newlist; verbose_output=false; // Make sure we don't do verbose output
          log.printf("  involving atoms : ");
          for(unsigned i=0;i<t.size();++i){ 
             newlist.push_back(i); addColvar( newlist ); newlist.clear();
             log.printf(" %d",t[i].serial() ); 
          }
          log.printf("\n");  
      } else {
          plumed_merror("SPECIES keyword is not for density or coordination like CV");
      }
  } else if( keywords.exists("SPECIESA") && keywords.exists("SPECIESB") ) {
      std::vector<AtomNumber> t1,t2;
      parseAtomList("SPECIESA",t1);
      if( !t1.empty() ){
         readatoms=true; 
         parseAtomList("SPECIESB",t2);
         if ( t2.empty() ) error("SPECIESB keyword defines no atoms or is missing. Use either SPECIESA and SPECIESB or just SPECIES");
         natoms=1+t2.size();
         for(unsigned i=0;i<t1.size();++i) all_atoms.addIndexToList( t1[i] );
         for(unsigned i=0;i<t2.size();++i) all_atoms.addIndexToList( t2[i] );
         std::vector<unsigned> newlist;
         for(unsigned i=0;i<t1.size();++i){
            newlist.push_back(i);
            for(unsigned j=0;j<t2.size();++j) newlist.push_back( t1.size() + j ); 
            addColvar( newlist ); newlist.clear();
         }
         if( !verbose_output ){
             log.printf("  generating colvars from a group of %d central atoms and %d other atoms\n",t1.size(), t2.size() );
             log.printf("  central atoms are : ");
             for(unsigned i=0;i<t1.size();++i) log.printf("%d ",t1[i].serial() );
             log.printf("\n");
             log.printf("  other atoms are : ");
             for(unsigned i=0;i<t2.size();++i) log.printf("%d ",t2[i].serial() );
             log.printf("\n");
         }
      }
  } 
}

void MultiColvar::resizeDynamicArrays(){
  for(unsigned i=0;i<taskList.getNumberActive();++i){
      unsigned n=taskList[i];
      activateLinks( colvar_atoms[n], all_atoms );
  }
  
  all_atoms.updateActiveMembers(); 
  ActionAtomistic::requestAtoms( all_atoms.retrieveActiveList() );
  forcesToApply.resize( getNumberOfDerivatives() );
}

void MultiColvar::prepare(){
  updatetime=false;
  if( contributorsAreUnlocked ){
      taskList.mpi_gatherActiveMembers( comm );
      mpi_gatherActiveMembers( comm, colvar_atoms ); 
      lockContributors(); updatetime=true;
  }
  if( updateFreq>0 && (getStep()-lastUpdate)>=updateFreq ){
      taskList.activateAll(); 
      for(unsigned i=0;i<taskList.getNumberActive();++i) colvar_atoms[i].activateAll();
      unlockContributors(); updatetime=true; lastUpdate=getStep();
  }
  if(updatetime){
     resizeDynamicArrays();
     resizeFunctions(); 
  }
}

void MultiColvar::calculate(){
  runAllTasks();
}

Vector MultiColvar::getCentralAtom(){
   plumed_merror("gradient and related cv distribution functions are not available in this colvar");
   Vector dummy; return dummy;
}

void MultiColvar::setAlignedPositions( const std::vector<Vector>& apos ){
  plumed_dbg_assert( apos.size()==pos.size() );
  for(unsigned i=0;i<pos.size();++i) pos[i]=apos[i];
}

const std::vector<Vector>& MultiColvar::getPositions(){
  if( !posHasBeenSet ){ 
     unsigned natoms=colvar_atoms[current].getNumberActive();
     // Resize everything
     if( pos.size()!=natoms ) pos.resize(natoms); 
     // Get the position
     for(unsigned i=0;i<natoms;++i) pos[i]=ActionAtomistic::getPosition( getAtomIndex(i) );  
     posHasBeenSet=true; 
  }
  return pos;
}

void MultiColvar::performTask( const unsigned& j ){
  atoms_with_derivatives.deactivateAll();            // Currently no atoms have derivatives
  if( colvar_atoms[current].getNumberActive()==0 ){  // Do nothing if there are no active atoms in the colvar
     setElementValue(1,0.0);
     return;                      
  }
  posHasBeenSet=false;                               // This ensures we don't set the pos array more than once per step  

  // Do a quick check on the size of this contribution  
  calculateWeight();
  if( getElementValue(1)<getTolerance() ) return;   

  // Compute everything
  double vv=compute( j ); 
  // Set the value of this element in ActionWithVessel
  setElementValue( 0, vv );
  // And finally gather the list of active atoms
  atoms_with_derivatives.updateActiveMembers();
  return;
}

Vector MultiColvar::retrieveCentralAtomPos( const bool& frac ){
  centralAtomDerivativesAreInFractional=frac; 
  ibox=getPbc().getInvBox().transpose();

  // Prepare to retrieve central atom
  for(unsigned i=0;i<atomsWithCatomDer.getNumberActive();++i) central_derivs[ atomsWithCatomDer[i] ].zero(); 
  atomsWithCatomDer.deactivateAll();   

  // Retrieve the central atom position
  Vector catom_pos;
  if(frac) catom_pos=getPbc().realToScaled( getCentralAtom() );
  else catom_pos=getCentralAtom();

  // Complete the update of the active member list
  atomsWithCatomDer.updateActiveMembers();
  return catom_pos;
}

void MultiColvar::addCentralAtomDerivatives( const unsigned& iatom, const Tensor& der ){
  plumed_dbg_assert( iatom<colvar_atoms[current].getNumberActive() );
  atomsWithCatomDer.activate(iatom);
  if( centralAtomDerivativesAreInFractional ) central_derivs[iatom] += matmul( ibox, der );
  else central_derivs[iatom] += der;
}

double MultiColvar::getCentralAtomDerivative( const unsigned& iatom, const unsigned jcomp, const Vector& df ) const {
  plumed_dbg_assert( iatom<colvar_atoms[current].getNumberActive() && jcomp<3 );
  return df[0]*central_derivs[iatom](0,jcomp) + df[1]*central_derivs[iatom](1,jcomp) + df[2]*central_derivs[iatom](2,jcomp);
}

Vector MultiColvar::getSeparation( const Vector& vec1, const Vector& vec2 ) const {
  if(usepbc){ return pbcDistance( vec1, vec2 ); }
  else{ return delta( vec1, vec2 ); }
}

void MultiColvar::apply(){
  getForcesFromVessels( forcesToApply );
  setForcesOnAtoms( forcesToApply );
}

void MultiColvar::mergeDerivatives( const unsigned& ider, const double& df ){
  unsigned vstart=getNumberOfDerivatives()*ider;
  for(unsigned i=0;i<atoms_with_derivatives.getNumberActive();++i){
     unsigned iatom=3*getAtomIndex( atoms_with_derivatives[i] );
     accumulateDerivative( iatom, df*getElementDerivative(vstart+iatom) ); iatom++;
     accumulateDerivative( iatom, df*getElementDerivative(vstart+iatom) ); iatom++;
     accumulateDerivative( iatom, df*getElementDerivative(vstart+iatom) );
  }
  unsigned nvir=3*getNumberOfAtoms();
  for(unsigned j=0;j<9;++j){
     accumulateDerivative( nvir, df*getElementDerivative(vstart+nvir) ); nvir++;
  }
}

void MultiColvar::clearDerivativesAfterTask( const unsigned& ider ){
  unsigned vstart=getNumberOfDerivatives()*ider; 
  for(unsigned i=0;i<atoms_with_derivatives.getNumberActive();++i){
     unsigned iatom=vstart+3*getAtomIndex( atoms_with_derivatives[i] );
     setElementDerivative( iatom, 0.0 ); iatom++;
     setElementDerivative( iatom, 0.0 ); iatom++;
     setElementDerivative( iatom, 0.0 );
  }   
  unsigned nvir=vstart+3*getNumberOfAtoms();
  for(unsigned j=0;j<9;++j){
     setElementDerivative( nvir, 0.0 ); nvir++;
  }
}

void MultiColvar::buildDerivativeIndexArrays( std::vector< DynamicList<unsigned> >& active_der ){
  // Clear old derivative indexes
  for(unsigned i=0;i<active_der.size();++i) active_der[i].clear();

  // Build indexes
  active_der.resize( colvar_atoms.size() );
  for(unsigned i=0;i<colvar_atoms.size();++i){
     for(unsigned j=0;j<colvar_atoms[i].fullSize();++j){
         unsigned jatom=linkIndex( j, colvar_atoms[i], all_atoms );
         active_der[i].addIndexToList( 3*jatom + 0 );
         active_der[i].addIndexToList( 3*jatom + 1 );
         active_der[i].addIndexToList( 3*jatom + 2 );
     }
     unsigned virs=3*getNumberOfAtoms();
     for(unsigned j=0;j<9;++j){
        active_der[i].addIndexToList( virs ); virs++;
     } 
  } 
}

StoreCentralAtomsVessel* MultiColvar::getCentralAtoms(){
  // Look to see if vectors have already been created
  StoreCentralAtomsVessel* mycatoms;
  for(unsigned i=0;i<getNumberOfVessels();++i){
     mycatoms=dynamic_cast<StoreCentralAtomsVessel*>( getPntrToVessel(i) );
     if( mycatoms ) return mycatoms;
  }

  // Create the vessel
  vesselbase::VesselOptions da("","",0,"",this); 
  StoreCentralAtomsVessel* sv=new StoreCentralAtomsVessel(da);
  addVessel(sv); resizeFunctions(); // This makes sure resizing of vessels is done
  return sv;
}
     
}
}
