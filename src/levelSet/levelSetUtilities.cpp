#include "levelSetUtilities.hpp"
#include <petsc.h>
#include <memory>
#include <algorithm>
#include "mathFunctions/functionWrapper.hpp"
#include "utilities/petscUtilities.hpp"
#include "utilities/mathUtilities.hpp"
#include "utilities/constants.hpp"
#include "levelSetUtilities.hpp"
#include "LS-VOF.hpp"
#include "cellGrad.hpp"
#include "domain/range.hpp"
#include "domain/reverseRange.hpp"
#include "mathFunctions/functionWrapper.hpp"
#include "utilities/constants.hpp"
#include "utilities/mathUtilities.hpp"
#include "utilities/petscSupport.hpp"
#include "utilities/petscUtilities.hpp"

void ablate::levelSet::Utilities::CellValGrad(DM dm, const PetscInt p, PetscReal *c, PetscReal *c0, PetscReal *g) {
    DMPolytopeType ct;
    PetscInt Nc;
    PetscReal *coords = NULL;
    const PetscScalar *array;
    PetscBool isDG;
    PetscReal x0[3];

    // Coordinates of the cell vertices
    DMPlexGetCellCoordinates(dm, p, &isDG, &Nc, &array, &coords) >> ablate::utilities::PetscUtilities::checkError;

    // Center of the cell
    DMPlexComputeCellGeometryFVM(dm, p, NULL, x0, NULL) >> ablate::utilities::PetscUtilities::checkError;

    // Get the cell type and call appropriate VOF function
    DMPlexGetCellType(dm, p, &ct) >> ablate::utilities::PetscUtilities::checkError;
    switch (ct) {
        case DM_POLYTOPE_SEGMENT:
            Grad_1D(x0, coords, c, c0, g) >> ablate::utilities::PetscUtilities::checkError;
            break;
        case DM_POLYTOPE_TRIANGLE:
            Grad_2D_Tri(x0, coords, c, c0, g) >> ablate::utilities::PetscUtilities::checkError;
            break;
        case DM_POLYTOPE_QUADRILATERAL:
            Grad_2D_Quad(x0, coords, c, c0, g) >> ablate::utilities::PetscUtilities::checkError;
            break;
        case DM_POLYTOPE_TETRAHEDRON:
            Grad_3D_Tetra(x0, coords, c, c0, g) >> ablate::utilities::PetscUtilities::checkError;
            break;
        case DM_POLYTOPE_HEXAHEDRON:
            Grad_3D_Hex(x0, coords, c, c0, g) >> ablate::utilities::PetscUtilities::checkError;
            break;
        default:
            throw std::invalid_argument("No element geometry for cell " + std::to_string(p) + " with type " + DMPolytopeTypes[ct]);
    }

    DMPlexRestoreCellCoordinates(dm, p, &isDG, &Nc, &array, &coords) >> ablate::utilities::PetscUtilities::checkError;
}

void ablate::levelSet::Utilities::CellValGrad(DM dm, const PetscInt fid, const PetscInt p, Vec f, PetscReal *c0, PetscReal *g) {
    PetscInt nv, *verts;
    const PetscScalar *fvals, *v;
    PetscScalar *c;

    DMPlexCellGetVertices(dm, p, &nv, &verts) >> ablate::utilities::PetscUtilities::checkError;

    DMGetWorkArray(dm, nv, MPIU_SCALAR, &c) >> ablate::utilities::PetscUtilities::checkError;

    VecGetArrayRead(f, &fvals) >> utilities::PetscUtilities::checkError;

    for (PetscInt i = 0; i < nv; ++i) {
        // DMPlexPointLocalFieldRead isn't behaving like I would expect. If I don't make f a pointer then it just returns zero.
        //    Additionally, it looks like it allows for the editing of the value.
        if (fid >= 0) {
            DMPlexPointLocalFieldRead(dm, verts[i], fid, fvals, &v) >> utilities::PetscUtilities::checkError;
        } else {
            DMPlexPointLocalRead(dm, verts[i], fvals, &v) >> utilities::PetscUtilities::checkError;
        }

        c[i] = *v;
    }

    DMPlexCellRestoreVertices(dm, p, &nv, &verts) >> ablate::utilities::PetscUtilities::checkError;

    ablate::levelSet::Utilities::CellValGrad(dm, p, c, c0, g);

    DMRestoreWorkArray(dm, nv, MPIU_SCALAR, &c) >> ablate::utilities::PetscUtilities::checkError;
}

void ablate::levelSet::Utilities::CellValGrad(std::shared_ptr<ablate::domain::SubDomain> subDomain, const ablate::domain::Field *field, const PetscInt p, PetscReal *c0, PetscReal *g) {
    DM dm = subDomain->GetFieldDM(*field);
    Vec f = subDomain->GetVec(*field);
    ablate::levelSet::Utilities::CellValGrad(dm, field->id, p, f, c0, g);
}

void ablate::levelSet::Utilities::VertexToVertexGrad(std::shared_ptr<ablate::domain::SubDomain> subDomain, const ablate::domain::Field *field, const PetscInt p, PetscReal *g) {
    // Given a field determine the gradient at a vertex

    DM dm = subDomain->GetFieldDM(*field);
    Vec vec = subDomain->GetVec(*field);

    DMPlexVertexGradFromVertex(dm, p, vec, field->id, 0, g) >> ablate::utilities::PetscUtilities::checkError;
}

// Given a level set and normal at the cell center compute the level set values at the vertices assuming a straight interface
void ablate::levelSet::Utilities::VertexLevelSet_LS(DM dm, const PetscInt p, const PetscReal c0, const PetscReal *n, PetscReal **c) {
    PetscInt dim, Nc, nVerts, i, j;
    PetscReal x0[3] = {0.0, 0.0, 0.0};
    PetscReal *coords = NULL;
    const PetscScalar *array;
    PetscBool isDG;

    DMGetDimension(dm, &dim) >> ablate::utilities::PetscUtilities::checkError;

    // The cell center
    DMPlexComputeCellGeometryFVM(dm, p, NULL, x0, NULL) >> ablate::utilities::PetscUtilities::checkError;

    // Coordinates of the cell vertices
    DMPlexGetCellCoordinates(dm, p, &isDG, &Nc, &array, &coords) >> ablate::utilities::PetscUtilities::checkError;

    // Number of vertices
    nVerts = Nc / dim;

    if (*c == NULL) {
        PetscMalloc1(nVerts, c) >> ablate::utilities::PetscUtilities::checkError;
    }

    // The level set value of each vertex. This assumes that the interface is a line/plane
    //    with the given unit normal.
    for (i = 0; i < nVerts; ++i) {
        (*c)[i] = c0;
        for (j = 0; j < dim; ++j) {
            (*c)[i] += n[j] * (coords[i * dim + j] - x0[j]);
        }
    }

    DMPlexRestoreCellCoordinates(dm, p, &isDG, &Nc, &array, &coords) >> ablate::utilities::PetscUtilities::checkError;
}

// Given a cell VOF and normal at the cell center compute the level set values at the vertices assuming a straight interface
void ablate::levelSet::Utilities::VertexLevelSet_VOF(DM dm, const PetscInt p, const PetscReal targetVOF, const PetscReal *n, PetscReal **c) {
    PetscReal vof;         // current VOF of the cell
    PetscReal area;        // length (2D) or area (3D) of the cell face
    PetscReal cellVolume;  // Area (2D) or volume (3D) of the cell
    const PetscReal tol = 1e-8;
    PetscInt i;
    PetscReal offset;
    PetscReal vofError;
    PetscInt nv;

    // Get the number of vertices for the cell
    DMPlexCellGetNumVertices(dm, p, &nv) >> ablate::utilities::PetscUtilities::checkError;

    // Get an initial guess at the vertex level set values assuming that the interface passes through the cell-center.
    // Also allocates c if c==NULL on entry
    ablate::levelSet::Utilities::VertexLevelSet_LS(dm, p, 0.0, n, c);

    // Get the resulting VOF from the initial guess
    ablate::levelSet::Utilities::VOF(dm, p, *c, &vof, &area, &cellVolume);
    vofError = targetVOF - vof;

    while (fabs(vofError) > tol) {
        // The amount the center level set value needs to shift by.
        offset = vofError * cellVolume / area;

        // If this isn't damped then it will overshoot and there will be no interface in the cell
        offset *= 0.5;

        for (i = 0; i < nv; ++i) {
            (*c)[i] -= offset;
        }

        ablate::levelSet::Utilities::VOF(dm, p, *c, &vof, &area, NULL);
        vofError = targetVOF - vof;
    };
}

// Returns the VOF for a given cell using the level-set values at the cell vertices.
// Refer to "Quadrature rules for triangular and tetrahedral elements with generalized functions"
//  by Holdych, Noble, and Secor, Int. J. Numer. Meth. Engng 2008; 73:1310-1327.
void ablate::levelSet::Utilities::VOF(DM dm, const PetscInt p, PetscReal *c, PetscReal *vof, PetscReal *area, PetscReal *vol) {
    DMPolytopeType ct;
    PetscInt Nc;
    PetscReal *coords = NULL;
    const PetscScalar *array;
    PetscBool isDG;

    // Coordinates of the cell vertices
    DMPlexGetCellCoordinates(dm, p, &isDG, &Nc, &array, &coords) >> ablate::utilities::PetscUtilities::checkError;

    // Get the cell type and call appropriate VOF function
    DMPlexGetCellType(dm, p, &ct) >> ablate::utilities::PetscUtilities::checkError;
    switch (ct) {
        case DM_POLYTOPE_SEGMENT:
            VOF_1D(coords, c, vof, area, vol);
            break;
        case DM_POLYTOPE_TRIANGLE:
            VOF_2D_Tri(coords, c, vof, area, vol);
            break;
        case DM_POLYTOPE_QUADRILATERAL:
            VOF_2D_Quad(coords, c, vof, area, vol);
            break;
        case DM_POLYTOPE_TETRAHEDRON:
            VOF_3D_Tetra(coords, c, vof, area, vol);
            break;
        case DM_POLYTOPE_HEXAHEDRON:
            VOF_3D_Hex(coords, c, vof, area, vol);
            break;
        default:
            throw std::invalid_argument("No element geometry for cell " + std::to_string(p) + " with type " + DMPolytopeTypes[ct]);
    }

    DMPlexRestoreCellCoordinates(dm, p, &isDG, &Nc, &array, &coords) >> ablate::utilities::PetscUtilities::checkError;
}

// Returns the VOF for a given cell with a known level set value (c0) and normal (nIn).
//  This computes the level-set values at the vertices by approximating the interface as a straight-line with the same normal
//  as provided
void ablate::levelSet::Utilities::VOF(DM dm, const PetscInt p, const PetscReal c0, const PetscReal *nIn, PetscReal *vof, PetscReal *area, PetscReal *vol) {
    PetscReal *c = NULL;
    ablate::levelSet::Utilities::VertexLevelSet_LS(dm, p, c0, nIn, &c);

    ablate::levelSet::Utilities::VOF(dm, p, c, vof, area, vol);  // Do the actual calculation.

    PetscFree(c) >> ablate::utilities::PetscUtilities::checkError;
}

// Returns the VOF for a given cell using an analytic level set equation
// Refer to "Quadrature rules for triangular and tetrahedral elements with generalized functions"
void ablate::levelSet::Utilities::VOF(DM dm, PetscInt p, const std::shared_ptr<ablate::mathFunctions::MathFunction> &phi, PetscReal *vof, PetscReal *area, PetscReal *vol) {
    PetscInt dim, Nc, nVerts, i;
    PetscReal *c = NULL, *coords = NULL;
    const PetscScalar *array;
    PetscBool isDG;

    DMGetDimension(dm, &dim) >> ablate::utilities::PetscUtilities::checkError;

    // Coordinates of the cell vertices
    DMPlexGetCellCoordinates(dm, p, &isDG, &Nc, &array, &coords) >> ablate::utilities::PetscUtilities::checkError;

    // Number of vertices
    nVerts = Nc / dim;

    PetscMalloc1(nVerts, &c) >> ablate::utilities::PetscUtilities::checkError;

    // The level set value of each vertex. This assumes that the interface is a line/plane
    //    with the given unit normal.
    for (i = 0; i < nVerts; ++i) {
        c[i] = phi->Eval(&coords[i * dim], dim, 0.0);
    }

    DMPlexRestoreCellCoordinates(dm, p, &isDG, &Nc, &array, &coords) >> ablate::utilities::PetscUtilities::checkError;

    ablate::levelSet::Utilities::VOF(dm, p, c, vof, area, vol);  // Do the actual calculation.

    PetscFree(c) >> ablate::utilities::PetscUtilities::checkError;
}

// Return the VOF in a cell where the level set is defined at vertices
void ablate::levelSet::Utilities::VOF(std::shared_ptr<ablate::domain::SubDomain> subDomain, PetscInt cell, const ablate::domain::Field *lsField, PetscReal *vof, PetscReal *area, PetscReal *vol) {
    DM dm = subDomain->GetFieldDM(*lsField);
    Vec vec = subDomain->GetVec(*lsField);
    const PetscScalar *array;
    PetscReal *c;

    PetscInt nv, *verts;
    DMPlexCellGetVertices(dm, cell, &nv, &verts) >> ablate::utilities::PetscUtilities::checkError;
    DMGetWorkArray(dm, nv, MPI_REAL, &c);

    VecGetArrayRead(vec, &array) >> ablate::utilities::PetscUtilities::checkError;
    for (PetscInt i = 0; i < nv; ++i) {
        const PetscReal *val;
        xDMPlexPointLocalRead(dm, verts[i], lsField->id, array, &val) >> ablate::utilities::PetscUtilities::checkError;
        c[i] = *val;
    }
    VecRestoreArrayRead(vec, &array) >> ablate::utilities::PetscUtilities::checkError;

    ablate::levelSet::Utilities::VOF(dm, cell, c, vof, area, vol);

    DMRestoreWorkArray(dm, nv, MPI_REAL, &c);
    DMPlexCellRestoreVertices(dm, cell, &nv, &verts) >> ablate::utilities::PetscUtilities::checkError;
}




void SaveVertexData(const char fname[255], const ablate::domain::Field *field, std::shared_ptr<ablate::domain::SubDomain> subDomain) {

  ablate::domain::Range range;
  PetscReal    *array, *val;
  Vec           vec = subDomain->GetVec(*field);
  DM            dm  = subDomain->GetFieldDM(*field);
  PetscInt      dim = subDomain->GetDimensions();

  ablate::domain::GetRange(dm, nullptr, 0, range);

  VecGetArray(vec, &array) >> ablate::utilities::PetscUtilities::checkError;

  FILE *f1 = fopen(fname, "w");

  for (PetscInt v = range.start; v < range.end; ++v) {
    PetscInt vert = range.points ? range.points[v] : v;
    PetscScalar *coords;

    DMPlexPointLocalFieldRef(dm, vert, field->id, array, &val) >> ablate::utilities::PetscUtilities::checkError;

    DMPlexVertexGetCoordinates(dm, 1, &vert, &coords);

    for (PetscInt d = 0; d < dim; ++d) {
      fprintf(f1, "%+.16e\t", coords[d]);
    }
    fprintf(f1, "%+.16ef\n", *val);

    DMPlexVertexRestoreCoordinates(dm, 1, &vert, &coords);
  }

  fclose(f1);

  VecRestoreArray(vec, &array) >> ablate::utilities::PetscUtilities::checkError;
  ablate::domain::RestoreRange(range);
}




  
  // Handle the case where the signs are different
  void checkSigns(PetscReal existingVal, PetscReal newVal) {
	  if (existingVal != PETSC_MAX_REAL && ((existingVal > 0 && newVal < 0) || (existingVal < 0 && newVal > 0))) {
		  PetscPrintf(PETSC_COMM_SELF, "Error: Different signs for shared vertex.\n");
      }
  }

// Compute the level-set field that corresponds to a given VOF field
// The steps are:
//  1 - Determine the level-set field in cells containing a VOF value between 0 and 1
//  2 - Mark the required number of vertices (based on the cells) next to the interface cells
//  3 - Iterate over vertices EXCEPT for those with cut-cells until converged
//  4 - We may want to look at a fourth step which improve the accuracy
void ablate::levelSet::Utilities::Reinitialize(std::shared_ptr<ablate::domain::rbf::RBF> rbf, std::shared_ptr<ablate::domain::SubDomain> subDomain, const ablate::domain::Field *vofField, const PetscInt nLevels, const ablate::domain::Field *lsField) {

  // Note: Need to write a unit test where the vof and ls fields aren't in the same DM, e.g. one is a SOL vector and one is an AUX vector.

  DM vofDM = subDomain->GetFieldDM(*vofField);
  DM lsDM = subDomain->GetFieldDM(*lsField);
  const PetscInt lsID = lsField->id;
  const PetscInt vofID = vofField->id;
  Vec vofVec = subDomain->GetVec(*vofField);
  Vec lsVec = subDomain->GetVec(*lsField);
  Vec ls_oldVal;
  PetscScalar *lsArray;
  const PetscScalar *vofArray;
  const PetscInt dim = subDomain->GetDimensions();   // VOF and LS subdomains must have the same dimension. Can't think of a reason they wouldn't.


  ablate::domain::Range cellRange, vertRange;
  subDomain->GetCellRange(nullptr, cellRange);
  subDomain->GetRange(nullptr, 0, vertRange);


  // Get the point->index mapping for cells
  ablate::domain::ReverseRange reverseVertRange = ablate::domain::ReverseRange(vertRange);
  ablate::domain::ReverseRange reverseCellRange = ablate::domain::ReverseRange(cellRange);


  PetscInt *vertMask, *cellMask;
  DMGetWorkArray(lsDM, vertRange.end - vertRange.start, MPIU_INT, &vertMask) >> ablate::utilities::PetscUtilities::checkError;
  DMGetWorkArray(vofDM, cellRange.end - cellRange.start, MPIU_INT, &cellMask) >> ablate::utilities::PetscUtilities::checkError;
  vertMask -= vertRange.start; // offset so that we can use start->end
  cellMask -= cellRange.start; // offset so that we can use start->end
  for (PetscInt i = vertRange.start; i < vertRange.end; ++i) {
    vertMask[i] = -1; //  Ignore the vertex
  }
  for (PetscInt i = cellRange.start; i < cellRange.end; ++i) {
    cellMask[i] = -1; // Ignore the cell
  }

     

  VecGetArray(lsVec, &lsArray) >> ablate::utilities::PetscUtilities::checkError;
  for (PetscInt v = vertRange.start; v < vertRange.end; ++v) {
    PetscInt vert = vertRange.GetPoint(v);
    PetscScalar *val;
    xDMPlexPointLocalRef(lsDM, vert, lsID, lsArray, &val) >> ablate::utilities::PetscUtilities::checkError;
    *val = PETSC_MAX_REAL;
  }

  VecGetArrayRead(vofVec, &vofArray) >> ablate::utilities::PetscUtilities::checkError;
  
  // Declare an array for iterator
  std::vector<PetscInt> lsIter(vertRange.end - vertRange.start, 0);
  PetscInt cutcellCount = 0;
  for (PetscInt c = cellRange.start; c < cellRange.end; ++c) {
    PetscInt cell = cellRange.GetPoint(c);

    PetscScalar *vofVal;
    xDMPlexPointLocalRead(vofDM, cell, vofID, vofArray, &vofVal) >> ablate::utilities::PetscUtilities::checkError;

    // Only worry about cut-cells
    if ( ((*vofVal) > ablate::utilities::Constants::small) && ((*vofVal) < (1.0 - ablate::utilities::Constants::small)) ) {

      cellMask[c] = 0;  // Mark as a cut-cell
      cutcellCount +=1;

      // Unit normal estimate
      PetscReal n[3];
      n[0] = -(rbf->EvalDer(vofField, cell, 1, 0, 0));
      n[1] = -(rbf->EvalDer(vofField, cell, 0, 1, 0));
      n[2] = -(dim==3 ? rbf->EvalDer(vofField, cell, 0, 0, 1) : 0.0);
      ablate::utilities::MathUtilities::NormVector(dim, n);

      PetscInt nv, *verts;
      DMPlexCellGetVertices(vofDM, cell, &nv, &verts) >> ablate::utilities::PetscUtilities::checkError;

      PetscReal *lsVertVals = NULL;
      //PetscReal vof_sum;
      //PetscReal alpha = 1.0;
      DMGetWorkArray(vofDM, nv, MPIU_REAL, &lsVertVals) >> ablate::utilities::PetscUtilities::checkError;

      // Level set values at the vertices
      ablate::levelSet::Utilities::VertexLevelSet_VOF(vofDM, cell, *vofVal, n, &lsVertVals);


      for (PetscInt v = 0; v < nv; ++v) {

        // Mark as a cut-cell vertex
        vertMask[reverseVertRange.GetIndex(verts[v])] = 0; // Mark vertex as associated with a cut-cell

        PetscScalar *lsVal;
        xDMPlexPointLocalRef(lsDM, verts[v], lsID, lsArray, &lsVal) >> ablate::utilities::PetscUtilities::checkError;
        
//-----------------------------------smallest value----------------------------------------//
/*		if (PetscAbsReal(lsVertVals[v]) < PetscAbsScalar(*lsVal)){ 
			checkSigns(*lsVal, lsVertVals[v]);
			*lsVal = lsVertVals[v];
        }*/
        
//--------------------------approach 2-------Update values based on average between just two values//
/*		if (*lsVal == PETSC_MAX_REAL) { 
		    *lsVal = lsVertVals[v]; 
		} 
		else {
			checkSigns(*lsVal, lsVertVals[v]);
		    *lsVal = (lsVertVals[v] + *lsVal) * 0.5; 
		}*/

//--------------------------approach 3-------Update values based on average between  values//
		PetscInt vert_i = reverseVertRange.GetIndex(verts[v]);
        vert_i -= vertRange.start;
				    
		if (lsIter[vert_i] == 0) {
			*lsVal = lsVertVals[v];
			lsIter[vert_i]++; 
		} 
		else {
			*lsVal = (lsVertVals[v] + *lsVal * lsIter[vert_i]) / (lsIter[vert_i] + 1);
			lsIter[vert_i]++;
		}
	
//--------------------------approach 4------wieghted average//	
/*		if (*lsVal == PETSC_MAX_REAL) { 
		    *lsVal = lsVertVals[v];
		    vof_sum = *vofVal; 
		} 
		else {
		    *lsVal = alpha * (*vofVal * lsVertVals[v] + *lsVal * vof_sum) / (vof_sum + *vofVal); 
		}*/
		
//--------------------------approach 5------harmonic average//	
/*		PetscInt vert_i = reverseVertRange.GetIndex(verts[v]);
        vert_i -= vertRange.start;
        
		if (*lsVal == PETSC_MAX_REAL) { 
			*lsVal = lsVertVals[v];
			lsIter[vert_i]++; 
		}
		else {
			*lsVal = (lsIter[vert_i] + 1) / ((lsIter[vert_i] / *lsVal) + (1 / lsVertVals[v]));
			lsIter[vert_i]++;
		}*/
		
//--------------------------approach 5------geometric average//	
/*		PetscInt vert_i = reverseVertRange.GetIndex(verts[v]);
        vert_i -= vertRange.start;
        
		if (*lsVal == PETSC_MAX_REAL) { 
			*lsVal = lsVertVals[v];
			lsIter[vert_i]++; 
		}
		else {
			*lsVal = pow(pow(*lsVal, lsIter[vert_i]) * lsVertVals[v], (1 / (lsIter[vert_i] + 1)));
			lsIter[vert_i]++;
		}*/
	
      }
      
      DMRestoreWorkArray(vofDM, nv, MPIU_REAL, &lsVertVals) >> ablate::utilities::PetscUtilities::checkError;
      DMPlexCellRestoreVertices(vofDM, cell, &nv, &verts) >> ablate::utilities::PetscUtilities::checkError;

    }
  }

  // define a new vector for storing old values of ls
  VecDuplicate(lsVec, &ls_oldVal) >> ablate::utilities::PetscUtilities::checkError;
  VecCopy(lsVec, ls_oldVal) >> ablate::utilities::PetscUtilities::checkError;
  PetscReal maxError = 1.0;
  PetscInt i = 0;
  //PetscReal ls_tolerance = 0.1;
  
  while ( maxError > 1e-6 ) {
  //for (PetscInt i = 0; i <= 8; ++i) {
	//-------------------------------------recalculating the unit normal vector------------------------------------------------------//
	  PetscScalar normal[dim*cutcellCount];
	  PetscInt currentCutCell = 0;  // Introduce an index to track which cut-cell we're working with
	  
	  for (PetscInt c = cellRange.start; c < cellRange.end; ++c) {
	    PetscInt cell = cellRange.GetPoint(c);
	
	    PetscScalar *vofVal;
	    xDMPlexPointLocalRead(vofDM, cell, vofID, vofArray, &vofVal) >> ablate::utilities::PetscUtilities::checkError;
	
	    // Only worry about cut-cells
	    if ( ((*vofVal) > ablate::utilities::Constants::small) && ((*vofVal) < (1.0 - ablate::utilities::Constants::small)) ) {
	
	      cellMask[c] = 0;  // Mark as a cut-cell
	
	      // Calculate unit normal vector based on the updated level set values at the vertices
	      PetscScalar n_new[dim];
		  DMPlexCellGradFromVertex(lsDM, cell, lsVec, lsID, 0, n_new) >> ablate::utilities::PetscUtilities::checkError;
		  ablate::utilities::MathUtilities::NormVector(dim, n_new); 
		  
		  // Copy the n_new values to the appropriate location in normal
	      std::copy(n_new, n_new + dim, &normal[currentCutCell*dim]);
	      currentCutCell++;
		}
		
	  }
	
	//-------------------------------------new for loop for recalculating the level set------------------------------------------------------//
	  // Declare an array for iterator
      std::vector<PetscInt> lsIter(vertRange.end - vertRange.start, 0);
	  currentCutCell = 0;
	  for (PetscInt c = cellRange.start; c < cellRange.end; ++c) {
	    PetscInt cell = cellRange.GetPoint(c);
	
	    PetscScalar *vofVal;
	    xDMPlexPointLocalRead(vofDM, cell, vofID, vofArray, &vofVal) >> ablate::utilities::PetscUtilities::checkError;
	
	    // Only worry about cut-cells
	    if ( ((*vofVal) > ablate::utilities::Constants::small) && ((*vofVal) < (1.0 - ablate::utilities::Constants::small)) ) {
	
	      cellMask[c] = 0;  // Mark as a cut-cell
	
	      PetscInt nv, *verts;
	      DMPlexCellGetVertices(vofDM, cell, &nv, &verts) >> ablate::utilities::PetscUtilities::checkError;
	
	      PetscReal *lsVertVals = NULL;
	      DMGetWorkArray(vofDM, nv, MPIU_REAL, &lsVertVals) >> ablate::utilities::PetscUtilities::checkError;
	
	      // Level set values at the vertices based on new unit normal vectors
	      PetscScalar n_new[dim];
	      std::copy(&normal[currentCutCell*dim], &normal[(currentCutCell+1)*dim], n_new);
	      ablate::levelSet::Utilities::VertexLevelSet_VOF(vofDM, cell, *vofVal, n_new, &lsVertVals);
	      currentCutCell +=1;
	
	      for (PetscInt v = 0; v < nv; ++v) {
	
	        // Mark as a cut-cell vertex
	        vertMask[reverseVertRange.GetIndex(verts[v])] = 0; // Mark vertex as associated with a cut-cell
	
	        PetscScalar *lsVal;
	        xDMPlexPointLocalRef(lsDM, verts[v], lsID, lsArray, &lsVal) >> ablate::utilities::PetscUtilities::checkError;
	
	//--------------------------approach 3-------Update values based on average between  values//
			PetscInt vert_i = reverseVertRange.GetIndex(verts[v]);
	        vert_i -= vertRange.start;
					    
			if (lsIter[vert_i] == 0) {
				*lsVal = lsVertVals[v];
				lsIter[vert_i]++; 
			} 
			else {
				*lsVal = (lsVertVals[v] + *lsVal * lsIter[vert_i]) / (lsIter[vert_i] + 1);
				lsIter[vert_i]++;
			}
		
		
	      }
	      
	      DMRestoreWorkArray(vofDM, nv, MPIU_REAL, &lsVertVals) >> ablate::utilities::PetscUtilities::checkError;
	      DMPlexCellRestoreVertices(vofDM, cell, &nv, &verts) >> ablate::utilities::PetscUtilities::checkError;
	    }
	  }
	  
	  Vec ls_diff;  // Temporary vector to store the difference
	  VecDuplicate(lsVec, &ls_diff) >> ablate::utilities::PetscUtilities::checkError;
	  VecWAXPY(ls_diff, -1.0, lsVec, ls_oldVal) >> ablate::utilities::PetscUtilities::checkError;
	  VecNorm(ls_diff, NORM_INFINITY, &maxError) >> ablate::utilities::PetscUtilities::checkError;

	  VecDestroy(&ls_diff);  // Clean up the temporary vector
      printf("%d: %e\n", i, maxError);
	  VecCopy(lsVec, ls_oldVal) >> ablate::utilities::PetscUtilities::checkError;
	  i++;
	  	  
  }

  // Comment out the rest of the code so that we can focus on the cut-cells only
/*
  // Now mark all of the necessary neighboring vertices. Note that this can't be put into the previous loop as all of the vertices
  //    for the cut-cells won't be known yet.
  for (PetscInt c = cellRange.start; c < cellRange.end; ++c) {
    PetscInt cutCell = cellRange.GetPoint(c);

    // Only worry about cut-cells
    if ( cellMask[c]==0 ) {
      // Center of the cell
      PetscReal x0[3];
      DMPlexComputeCellGeometryFVM(vofDM, cutCell, NULL, x0, NULL) >> ablate::utilities::PetscUtilities::checkError;

      // Once the neightbor function for vertices is merged this will need to be moved over
      PetscInt nCells, *cells;
      DMPlexGetNeighbors(vofDM, cutCell, nLevels, -1.0, -1, PETSC_FALSE, PETSC_FALSE, &nCells, &cells) >> ablate::utilities::PetscUtilities::checkError;

      for (PetscInt i = 0; i < nCells; ++i) {

        PetscInt cellID = reverseCellRange.GetIndex(cells[i]);
        if (cellMask[cellID]<0) {
          cellMask[cellID] = 1; // Mark as a cell where cell-centered gradients are needed
        }

        PetscInt nv, *verts;
        DMPlexCellGetVertices(vofDM, cells[i], &nv, &verts) >> ablate::utilities::PetscUtilities::checkError;

        PetscScalar *coords;
        DMPlexVertexGetCoordinates(vofDM, nv, verts, &coords) >> ablate::utilities::PetscUtilities::checkError;

        for (PetscInt v = 0; v < nv; ++v) {
          PetscInt id = reverseVertRange.GetIndex(verts[v]);
          if (vertMask[id]<0) {
            vertMask[id] = 1;
          }

          if (vertMask[id]==1) {

            // As an initial guess at the signed-distance function use the distance from the cut-cell center to this vertex
            PetscReal dist = 0.0;
            for (PetscInt d = 0; d < dim; ++d) {
              dist += PetscSqr(x0[d] - coords[v*dim + d]);
            }
            dist = PetscSqrtReal(dist);

            PetscScalar *lsVal;
            xDMPlexPointLocalRef(lsDM, verts[v], lsID, lsArray, &lsVal) >> ablate::utilities::PetscUtilities::checkError;

            if (dist < PetscAbs(*lsVal)) {
              PetscScalar *vofVal;
              xDMPlexPointLocalRead(vofDM, cells[i], vofID, vofArray, &vofVal) >> ablate::utilities::PetscUtilities::checkError;
              PetscReal sgn = (*vofVal < 0.5 ? +1.0 : -1.0);
              *lsVal = sgn*dist;
            }
          }
        }

        DMPlexVertexRestoreCoordinates(vofDM, nv, verts, &coords) >> ablate::utilities::PetscUtilities::checkError;
        DMPlexCellRestoreVertices(vofDM, cells[i], &nv, &verts) >> ablate::utilities::PetscUtilities::checkError;
      }

      PetscFree(cells) >> ablate::utilities::PetscUtilities::checkError;

    }
  }

  // Set the vertices too-far away as the largest possible value in the domain with the appropriate sign
  PetscReal gMin[3], gMax[3], maxDist = -1.0;
  DMGetBoundingBox(lsDM, gMin, gMax) >> ablate::utilities::PetscUtilities::checkError;

  for (PetscInt d = 0; d < dim; ++d) {
    maxDist = PetscMax(maxDist, gMax[d] - gMin[d]);
  }
  for (PetscInt c = cellRange.start; c < cellRange.end; ++c) {
    PetscInt cell = cellRange.GetPoint(c);

    // Only worry about cells to far away
    if ( cellMask[c]==-1 ) {
      PetscScalar *vofVal;
      xDMPlexPointLocalRead(vofDM, cell, vofID, vofArray, &vofVal) >> ablate::utilities::PetscUtilities::checkError;

      PetscReal sgn = PetscSignReal(0.5 - (*vofVal));

      PetscInt nv, *verts;
      DMPlexCellGetVertices(vofDM, cell, &nv, &verts) >> ablate::utilities::PetscUtilities::checkError;

      for (PetscInt v = 0; v < nv; ++v) {
        PetscInt id = reverseVertRange.GetIndex(verts[v]);
        if (vertMask[id]<0) {
          PetscScalar *lsVal;
          xDMPlexPointLocalRef(lsDM, verts[v], lsID, lsArray, &lsVal) >> ablate::utilities::PetscUtilities::checkError;
          *lsVal = sgn*maxDist;
        }
      }
      DMPlexCellRestoreVertices(vofDM, cell, &nv, &verts) >> ablate::utilities::PetscUtilities::checkError;
    }
  }

  VecRestoreArrayRead(vofVec, &vofArray) >> ablate::utilities::PetscUtilities::checkError;

  // Create the mapping between DMPlex cell numbering and location in the array storing cell-centered gradients
  PetscInt numCells = 0;
  for (PetscInt c = cellRange.start; c < cellRange.end; ++c) {
    numCells += (cellMask[c]>-1);
  }

  PetscInt *cellArray, *indexArray;
  DMGetWorkArray(vofDM, numCells, MPIU_INT, &cellArray) >> ablate::utilities::PetscUtilities::checkError;
  DMGetWorkArray(vofDM, numCells, MPIU_INT, &indexArray) >> ablate::utilities::PetscUtilities::checkError;


  PetscInt i = 0;
  for (PetscInt c = cellRange.start; c < cellRange.end; ++c) {
    if (cellMask[c]>-1) {
      cellArray[i] = cellRange.GetPoint(c);
      indexArray[i] = i;
      ++i;

    }
  }
*/
  cellMask += cellRange.start;  // Reset the offset, otherwise DMRestoreWorkArray will return unexpected results
  DMRestoreWorkArray(vofDM, cellRange.end - cellRange.start, MPIU_INT, &cellMask) >> ablate::utilities::PetscUtilities::checkError;
  subDomain->RestoreRange(cellRange);
/*
  AO cellToIndex;
  AOCreateMapping(PETSC_COMM_SELF, numCells, cellArray, indexArray, &cellToIndex) >> ablate::utilities::PetscUtilities::checkError;

  PetscScalar *cellGradArray;
  DMGetWorkArray(vofDM, dim*numCells, MPIU_SCALAR, &cellGradArray) >> ablate::utilities::PetscUtilities::checkError;

  PetscReal h;
  DMPlexGetMinRadius(vofDM, &h) >> ablate::utilities::PetscUtilities::checkError;

  PetscReal diff = 1.0;
  PetscInt it = 0;
  while (diff>1e-2 && it<77e10) {
    ++it;
    for (PetscInt i = 0; i < numCells; ++i) {
      PetscInt cell = cellArray[i];

      ablate::levelSet::Utilities::CellValGrad(lsDM, lsID, cell, lsVec, nullptr, &(cellGradArray[i*dim]));

    }

    diff = 0.0;

    for (PetscInt v = vertRange.start; v < vertRange.end; ++v) {

      if (vertMask[v]==1) {
        PetscInt vert = vertRange.GetPoint(v);
        PetscReal g[dim];
        PetscReal *phi;

        xDMPlexPointLocalRef(lsDM, vert, lsID, lsArray, &phi) >> ablate::utilities::PetscUtilities::checkError;

        DMPlexVertexGradFromVertex(lsDM, vert, lsVec, lsID, g) >> ablate::utilities::PetscUtilities::checkError;

        VertexUpwindGrad(lsDM, cellGradArray, cellToIndex, vert, PetscSignReal(*phi), g);

        PetscReal mag = ablate::utilities::MathUtilities::MagVector(dim, g) - 1.0;

        PetscReal s = PetscSignReal(*phi);

        *phi -= h*s*mag;

        mag = PetscAbsReal(mag);
        diff = PetscMax(diff, mag);

      }
    }
    //printf("%e\n", diff);
  }


  DMRestoreWorkArray(vofDM, dim*numCells, MPIU_SCALAR, &cellGradArray) >> ablate::utilities::PetscUtilities::checkError;
  DMRestoreWorkArray(vofDM, numCells, MPIU_INT, &cellArray) >> ablate::utilities::PetscUtilities::checkError;
  DMRestoreWorkArray(vofDM, numCells, MPIU_INT, &indexArray) >> ablate::utilities::PetscUtilities::checkError;
  AODestroy(&cellToIndex) >> ablate::utilities::PetscUtilities::checkError;
*/

  vertMask += vertRange.start; // Reset the offset, otherwise DMRestoreWorkArray will return unexpected results
  DMRestoreWorkArray(lsDM, vertRange.end - vertRange.start, MPIU_INT, &vertMask) >> ablate::utilities::PetscUtilities::checkError;
  subDomain->RestoreRange(vertRange);


}