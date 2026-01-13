#ifndef RBTREE_H
#define RBTREE_H

#include <atomic>
#include <utility>
#include "ThreadData.h"

// Spinlock for thread safety
class Spinlock {
public:
    Spinlock() : _flag(false) {}

    void lock() {
        while (_flag.exchange(true, std::memory_order_acquire));
    }

    void unlock() {
        _flag.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> _flag;
};

enum Color { RED, BLACK };

struct Node {
    ThreadData* data;
    Color color;
    Node *parent, *left, *right;
    Node* minNode; // Augmented data

    Node(ThreadData* val) : data(val), color(RED), parent(nullptr), left(nullptr), right(nullptr), minNode(this) {}
};

class RBTree {
private:
    Node* root;
    mutable Spinlock _lock;

    void rotateLeft(Node* node);
    void rotateRight(Node* node);
    void fixInsert(Node* node);
    void fixDelete(Node* node);
    void transplant(Node* u, Node* v);
    Node* minValueNode(Node* node);
    Node* searchTreeHelper(Node* node, ThreadData* key);
    void deleteTree(Node* node);
    void updateMin(Node* node);

public:
    RBTree() : root(nullptr) {}
    ~RBTree() { deleteTree(root); }

    void insert(ThreadData* key);
    void remove(ThreadData* data);
    ThreadData* getMinimum() const;
    Node* searchTree(ThreadData* k);
    void clear();
};

void RBTree::updateMin(Node* node) {
    if (node == nullptr) return;

    node->minNode = node;
    if (node->left != nullptr && node->left->minNode->data->VirtualDeadline() < node->minNode->data->VirtualDeadline()) {
        node->minNode = node->left->minNode;
    }
    if (node->right != nullptr && node->right->minNode->data->VirtualDeadline() < node->minNode->data->VirtualDeadline()) {
        node->minNode = node->right->minNode;
    }
}

void RBTree::rotateLeft(Node* node) {
    Node* child = node->right;
    node->right = child->left;
    if (node->right != nullptr)
        node->right->parent = node;
    child->parent = node->parent;
    if (node->parent == nullptr)
        root = child;
    else if (node == node->parent->left)
        node->parent->left = child;
    else
        node->parent->right = child;
    child->left = node;
    node->parent = child;
    updateMin(node);
    updateMin(child);
}

void RBTree::rotateRight(Node* node) {
    Node* child = node->left;
    node->left = child->right;
    if (node->left != nullptr)
        node->left->parent = node;
    child->parent = node->parent;
    if (node->parent == nullptr)
        root = child;
    else if (node == node->parent->left)
        node->parent->left = child;
    else
        node->parent->right = child;
    child->right = node;
    node->parent = child;
    updateMin(node);
    updateMin(child);
}

void RBTree::fixInsert(Node* node) {
    Node* parent = nullptr;
    Node* grandparent = nullptr;
    while (node != root && node->color == RED && node->parent->color == RED) {
        parent = node->parent;
        grandparent = parent->parent;
        if (parent == grandparent->left) {
            Node* uncle = grandparent->right;
            if (uncle != nullptr && uncle->color == RED) {
                grandparent->color = RED;
                parent->color = BLACK;
                uncle->color = BLACK;
                node = grandparent;
            } else {
                if (node == parent->right) {
                    rotateLeft(parent);
                    node = parent;
                    parent = node->parent;
                }
                rotateRight(grandparent);
                std::swap(parent->color, grandparent->color);
                node = parent;
            }
        } else {
            Node* uncle = grandparent->left;
            if (uncle != nullptr && uncle->color == RED) {
                grandparent->color = RED;
                parent->color = BLACK;
                uncle->color = BLACK;
                node = grandparent;
            } else {
                if (node == parent->left) {
                    rotateRight(parent);
                    node = parent;
                    parent = node->parent;
                }
                rotateLeft(grandparent);
                std::swap(parent->color, grandparent->color);
                node = parent;
            }
        }
    }
    root->color = BLACK;
}

void RBTree::insert(ThreadData* key) {
    _lock.lock();
    Node* node = new Node(key);
    Node* parent = nullptr;
    Node* current = root;
    while (current != nullptr) {
        parent = current;
        if (node->data < current->data)
            current = current->left;
        else
            current = current->right;
    }
    node->parent = parent;
    if (parent == nullptr)
        root = node;
    else if (node->data < parent->data)
        parent->left = node;
    else
        parent->right = node;

    Node* temp = node;
    while(temp != nullptr) {
        updateMin(temp);
        temp = temp->parent;
    }

    fixInsert(node);
    _lock.unlock();
}

void RBTree::fixDelete(Node* node) {
     if (node == nullptr) return;
    while (node != root && node->color == BLACK) {
        if (node == node->parent->left) {
            Node* sibling = node->parent->right;
            if (sibling->color == RED) {
                sibling->color = BLACK;
                node->parent->color = RED;
                rotateLeft(node->parent);
                sibling = node->parent->right;
            }
            if ((sibling->left == nullptr || sibling->left->color == BLACK) &&
                (sibling->right == nullptr || sibling->right->color == BLACK)) {
                sibling->color = RED;
                node = node->parent;
            } else {
                if (sibling->right == nullptr || sibling->right->color == BLACK) {
                    if (sibling->left != nullptr)
                        sibling->left->color = BLACK;
                    sibling->color = RED;
                    rotateRight(sibling);
                    sibling = node->parent->right;
                }
                sibling->color = node->parent->color;
                node->parent->color = BLACK;
                if (sibling->right != nullptr)
                    sibling->right->color = BLACK;
                rotateLeft(node->parent);
                node = root;
            }
        } else {
            Node* sibling = node->parent->left;
            if (sibling->color == RED) {
                sibling->color = BLACK;
                node->parent->color = RED;
                rotateRight(node->parent);
                sibling = node->parent->left;
            }
            if ((sibling->left == nullptr || sibling->left->color == BLACK) &&
                (sibling->right == nullptr || sibling->right->color == BLACK)) {
                sibling->color = RED;
                node = node->parent;
            } else {
                if (sibling->left == nullptr || sibling->left->color == BLACK) {
                    if (sibling->right != nullptr)
                        sibling->right->color = BLACK;
                    sibling->color = RED;
                    rotateLeft(sibling);
                    sibling = node->parent->left;
                }
                sibling->color = node->parent->color;
                node->parent->color = BLACK;
                if (sibling->left != nullptr)
                    sibling->left->color = BLACK;
                rotateRight(node->parent);
                node = root;
            }
        }
    }
    if (node != nullptr)
        node->color = BLACK;
}

void RBTree::transplant(Node* u, Node* v) {
    if (u->parent == nullptr)
        root = v;
    else if (u == u->parent->left)
        u->parent->left = v;
    else
        u->parent->right = v;
    if (v != nullptr)
        v->parent = u->parent;
}

Node* RBTree::minValueNode(Node* node) {
    Node* current = node;
    while (current->left != nullptr)
        current = current->left;
    return current;
}

void RBTree::remove(ThreadData* data) {
    _lock.lock();
    Node* z = searchTree(data);
    if (z == nullptr) {
        _lock.unlock();
        return;
    }

    Node *x, *y;
    y = z;
    Color yOriginalColor = y->color;
    if (z->left == nullptr) {
        x = z->right;
        transplant(z, z->right);
    } else if (z->right == nullptr) {
        x = z->left;
        transplant(z, z->left);
    } else {
        y = minValueNode(z->right);
        yOriginalColor = y->color;
        x = y->right;
        if (y->parent == z) {
            if (x != nullptr)
                x->parent = y;
        } else {
            transplant(y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        transplant(z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }

    Node* temp = x ? x->parent : y->parent;
     if (y != z) {
        temp = y->parent;
    }


    delete z;

    if (yOriginalColor == BLACK) {
        fixDelete(x);
    }

    while(temp != nullptr) {
        updateMin(temp);
        temp = temp->parent;
    }
    _lock.unlock();
}


Node* RBTree::searchTreeHelper(Node* node, ThreadData* key) {
    if (node == nullptr || key == node->data) {
        return node;
    }

    if (key < node->data) {
        return searchTreeHelper(node->left, key);
    }
    return searchTreeHelper(node->right, key);
}

Node* RBTree::searchTree(ThreadData* k) {
    return searchTreeHelper(root, k);
}

ThreadData* RBTree::getMinimum() const {
    _lock.lock();
    if (root == nullptr) {
        _lock.unlock();
        return nullptr;
    }
    ThreadData* minData = root->minNode->data;
    _lock.unlock();
    return minData;
}

void RBTree::deleteTree(Node* node) {
    if (node != nullptr) {
        deleteTree(node->left);
        deleteTree(node->right);
        delete node;
    }
}

void RBTree::clear() {
    deleteTree(root);
    root = nullptr;
}

#endif // RBTREE_H