#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

// Student structure
typedef struct {
    int id;
    int helpCount;
} Student;

// Shared memory among threads
int numStudents;
int numTutors;
int numChairs;
int helpRequired;

int emptyChairs;
int waitingCount = 0;

long totalRequests = 0;
long totalSessionsCompleted = 0;
int activeSessions = 0;

Student* students = NULL;

int* studentTutorMapping = NULL;

// Locks
pthread_mutex_t chairsLock;
pthread_mutex_t queueLock;
pthread_mutex_t countersLock;
pthread_mutex_t arrivalLock;

// Semaphores
sem_t* studentSemaphores = NULL; // A semaphore per student
sem_t studentHasArrived;
sem_t studentIsWaitingForTutor;

int shutdownFlag = 0;

//Queue for student arrival (FIFO)
int* arrivalQueue = NULL;
int front = 0; //no one in front of line
int back = 0; //no one in back of line
int arrivalCapacity = 0; //# of students

void initializeArrivalQueue(int capacity) {
   arrivalQueue = malloc(sizeof(int) * capacity);
   front = 0;
   back = 0;
   arrivalCapacity = capacity;
}


void destroyArrivalQueue() {
free(arrivalQueue);
}

//Push the student who just arrived onto the waiting queue
void pushArrivalQueue(int studentId) {
   pthread_mutex_lock(&queueLock);
   arrivalQueue[back] = studentId;
   back = (back + 1) % arrivalCapacity;
   pthread_mutex_unlock(&queueLock);
}
//Pops a student from the front of the queue
int popArrivalQueue() {
pthread_mutex_lock(&queueLock);
if (front == back) { //if the queue is empty
 pthread_mutex_unlock(&queueLock);
 return -1;
}

int studentId = arrivalQueue[front];
front = (front + 1) % arrivalCapacity;
pthread_mutex_unlock(&queueLock);
return studentId;
}

// Priority queue for tutor

typedef struct PriorityNode {
int studentId;
int priority; // higher priorty will be seen first
struct PriorityNode* next;
} PriorityNode;

PriorityNode* priorityHead = NULL;

void initializePriorityQueue(int maxPriority) {
(void)maxPriority;
priorityHead = NULL;
}
//
void destroyPriorityQueue() {
while (priorityHead != NULL) {
	PriorityNode* tmp = priorityHead;
	priorityHead = priorityHead->next;
	free(tmp);
	}
}
//Pushes student into priority queue based on their priority
void pushPriorityQueue(int studentId, int priority) {
  PriorityNode* node = malloc(sizeof(PriorityNode));
      node->studentId = studentId;
          node->priority = priority;
	      node->next = NULL;

//Inserts the student at the head if it is empty or if the student has higher priority
if (!priorityHead || priority < priorityHead->priority || (priority == priorityHead->priority && studentId)) {
node->next = priorityHead;
priorityHead = node;
return;
}
PriorityNode* curr = priorityHead;
while(curr->next && (curr->next->priority < priority || (curr->next->priority == priority && curr->next->studentId < studentId))) {
curr = curr->next;
}
node->next = curr->next;
curr->next = node;
}
//Removes student from priority queue
int popPriorityQueue(int* studentId, int* priority) {
    if (!priorityHead) return 0; //returns 0 if empty
        PriorityNode* node = priorityHead;
	priorityHead = priorityHead->next;
	*studentId = node->studentId;
	*priority = node->priority;
	free(node);
	return 1;
}
//Counts the # of students in the priority queue
int sizePriorityQueue() {
int count = 0;
PriorityNode* curr = priorityHead;
while (curr) {
	count++;
	curr = curr->next;
	}
	return count;
}	
// Thread functions
void* studentThreadFunction(void* arg);
void* coordinatorThreadFunction(void* arg);
void* tutorThreadFunction(void* arg);

// parseInt util func
int parseInt(const char* s, int* out) {
    char* end = NULL;
    long val = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0') return -1;
    if (val <= 0 || val > 1000000) return -1;
    *out = (int) val;
    return 0;
}


// *** MAIN ***
int main(int argc, char* argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Number of arguments provided\n");
        return 1;
    }

    if (parseInt(argv[1], &numStudents) || parseInt(argv[2], &numTutors) || parseInt(argv[3], &numChairs) ||
        parseInt(argv[4], &helpRequired)) {
        fprintf(stderr, "Invalid argument(s)\n");
        return 1;
    }

    if (numChairs < 0) {
        fprintf(stderr, "Negative number of chairs\n");
        return 1;
    }

    emptyChairs = numChairs;

    // Allocate student structures in heap
    students = calloc(numStudents, sizeof(Student));
    studentSemaphores = calloc(numStudents, sizeof(sem_t));
    studentTutorMapping = calloc(numStudents, sizeof(int));

    if (!students || !studentSemaphores || !studentTutorMapping) {
        perror("calloc");
        return 1;
    }

    // Init semaphores
    for (int i = 0; i < numStudents; i++) {
        students[i].id = i;
        students[i].helpCount = 0;
        studentTutorMapping[i] = -1;
        if (sem_init(&studentSemaphores[i], 0, 0) != 0) { // 0 == blocking
            perror("sem_init studentSemaphores");
            return 1;
        }
    }

    if (sem_init(&studentHasArrived, 0, 0) != 0) { // 0 = blocking
        perror("sem_init studentHasArrived");
        return 1;
    }
    if (sem_init(&studentIsWaitingForTutor, 0, 0) != 0) { // 0 = blocking
        perror("sem_init studentIsWaitingForTutor");
        return 1;
    }

    initializePriorityQueue(helpRequired);
    initializeArrivalQueue(numStudents);

    srand((unsigned) time(NULL));

    // Initialize locks
    pthread_mutex_init(&countersLock, NULL);
    pthread_mutex_init(&chairsLock, NULL);
    pthread_mutex_init(&queueLock, NULL);
    pthread_mutex_init(&arrivalLock, NULL);

    // Create threads // TODO: Check for memory leaks
    pthread_t coordinatorTid;
    pthread_t* studentThreads = calloc(numStudents, sizeof(pthread_t));
    pthread_t* tutorThreads = calloc(numTutors, sizeof(pthread_t));

    if (!studentThreads || !tutorThreads) {
        perror("calloc thread arrays");
        return 1;
    }

    if (pthread_create(&coordinatorTid, NULL, coordinatorThreadFunction, NULL) != 0) {
        perror("pthread_create coordinator");
        return 1;
    }

    for (int i = 0; i < numStudents; i++) {
        if (pthread_create(&studentThreads[i], NULL, studentThreadFunction, &students[i]) != 0) {
            perror("pthread_create student");
            return 1;
        }
    }

    for (int i = 0; i < numTutors; i++) {
        int* tutorId = malloc(sizeof(int));
        if (!tutorId) {
            perror("malloc tutorId");
            return 1;
        }
        *tutorId = i;
        if (pthread_create(&tutorThreads[i], NULL, tutorThreadFunction, tutorId) != 0) {
            perror("pthread_create tutor");
            return 1;
        }
    }

    // Waiting for all student threads to finish...
    for (int i = 0; i < numStudents; i++) {
        pthread_join(studentThreads[i], NULL);
    }

    // Shutdown flag for safe exit
    shutdownFlag = 1;

    // Wake coordinator threads and tutor threads for exit...
    sem_post(&studentHasArrived);
    for (int i = 0; i < numTutors; i++) {
        sem_post(&studentIsWaitingForTutor);
    }

    pthread_join(coordinatorTid, NULL);
    for (int i = 0; i < numTutors; i++) {
        pthread_join(tutorThreads[i], NULL);
    }

    // Cleanup phase
    destroyPriorityQueue();
    destroyArrivalQueue();

 
    for (int i = 0; i < numStudents; i++) {
        sem_destroy(&studentSemaphores[i]);
    }

    sem_destroy(&studentHasArrived);
    sem_destroy(&studentIsWaitingForTutor);

    pthread_mutex_destroy(&chairsLock);
    pthread_mutex_destroy(&queueLock);
    pthread_mutex_destroy(&countersLock);

    free(studentThreads);
    free(tutorThreads);
    free(students);
    free(studentSemaphores);
    free(studentTutorMapping);

    return 0;
}

// *** STUDENT THREAD ***
void* studentThreadFunction(void* arg) {
    Student* s = arg;
    int id = s->id;

    while (1) {
        if (s->helpCount >= helpRequired) {
            break;
        }

        // Student is programming...
        usleep(rand() % 2000);

        pthread_mutex_lock(&chairsLock);
        if (emptyChairs > 0) { // If there are empty chairs available:
            // Take a chair
            emptyChairs--;
            waitingCount++;
            int chairsAfter = emptyChairs;
            pthread_mutex_unlock(&chairsLock);

            printf("S: Student %d takes a seat. Empty chairs remaining = %d\n", id, chairsAfter);

            pthread_mutex_lock(&arrivalLock);
            pushArrivalQueue(id);
            pthread_mutex_unlock(&arrivalLock);
            sem_post(&studentHasArrived);

            sem_wait(&studentSemaphores[id]);

            // Free waiting room chair ASAP
            pthread_mutex_lock(&chairsLock);
            emptyChairs++;
            waitingCount--;
            pthread_mutex_unlock(&chairsLock);

            int tutorId = studentTutorMapping[id];
            printf("S: Student %d receives help from Tutor %d\n", id, tutorId);

            // Student is being tutored...
            usleep(200);

            s->helpCount++;
        } else { // If there are no empty chairs available:
            pthread_mutex_unlock(&chairsLock);
            printf("S: Student %d found no empty chair. Will come again later.\n", id);
        }
    }

    return NULL;
}

// *** COORDINATOR THREAD ***
void* coordinatorThreadFunction(void* arg) {
    (void)arg; // unused param

    while (1) {
        sem_wait(&studentHasArrived);

        if (shutdownFlag) {
            break;
        }
        pthread_mutex_lock(&arrivalLock);
        int studentId = popArrivalQueue();
        pthread_mutex_unlock(&arrivalLock);
        if (studentId < 0) { // queue is empty
            continue;
        }

        int priority = students[studentId].helpCount + 1;

        // Place student in priority queue...
        pthread_mutex_lock(&queueLock);
        pushPriorityQueue(studentId, priority);
        int numStudentsWaitingForTutoring = sizePriorityQueue();

        pthread_mutex_lock(&countersLock);
        totalRequests++;
        long requestCount = totalRequests;
        pthread_mutex_unlock(&countersLock);

        pthread_mutex_unlock(&queueLock);

        printf("C: Student %d with priority %d in queue. Waiting students = %d. Total help requested so far = %ld\n",
            studentId, priority, numStudentsWaitingForTutoring, requestCount);

        sem_post(&studentIsWaitingForTutor);
    }

    return NULL;
}

// *** TUTOR THREAD ***
void* tutorThreadFunction(void* arg) {
    int tutorId = *(int*)arg;
    free(arg);

    while (1) {
        sem_wait(&studentIsWaitingForTutor);

        if (shutdownFlag) {
            break;
        }

        pthread_mutex_lock(&queueLock);
        int studentId;
        int priority;
        int isPriorityQueueNotEmpty = popPriorityQueue(&studentId, &priority);
        pthread_mutex_unlock(&queueLock);

        if (!isPriorityQueueNotEmpty) {
            continue;
        }

        pthread_mutex_lock(&countersLock);
        activeSessions++;
        totalSessionsCompleted++;
        int currentActive = activeSessions;
        long sessionCount = totalSessionsCompleted;
        pthread_mutex_unlock(&countersLock);

        studentTutorMapping[studentId] = tutorId;
        sem_post(&studentSemaphores[studentId]);

        printf("T: Student %d tutored by Tutor %d. Total sessions being tutored = %d. Total sessions tutored by all = %ld\n",
            studentId, tutorId, currentActive, sessionCount);

        usleep(200);

        pthread_mutex_lock(&countersLock);
        activeSessions--;
        pthread_mutex_unlock(&countersLock);
    }

    return NULL;
}
